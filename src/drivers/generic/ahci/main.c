/*
 * drivers/generic/ahci/main.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <scheduler/thread.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <locks/spinlock.h>
#include <locks/semaphore.h>

#include <utils/tree.h>

#include <irq/irq.h>

#include "structs.h"

#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <uacpi_libc.h>

#if defined(__x86_64__)
#include <arch/x86_64/ioapic.h>
#endif

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_WEAK OBOS_PAGEABLE_FUNCTION obos_status ioctl_var(size_t nParameters, uint64_t request, va_list list)
{
    OBOS_UNUSED(nParameters);
    OBOS_UNUSED(request);
    OBOS_UNUSED(list);
    return OBOS_STATUS_INVALID_IOCTL; // we don't support any
}
OBOS_WEAK OBOS_PAGEABLE_FUNCTION obos_status ioctl(size_t nParameters, uint64_t request, ...)
{
    va_list list;
    va_start(list, request);
    obos_status status = ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}
void driver_cleanup_callback()
{
}
__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_FLAGS_DETECT_VIA_PCI,
    .acpiId.nPnpIds = 0,
    .pciId.indiv = {
        .classCode = 0x01, // mass storage controller
        .subClass  = 0x06, // SATA Controller
        .progIf    = 0x01, // AHCI 1.0
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .ioctl_var = ioctl_var,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
    },
    .driverName = "AHCI Driver"
};

volatile HBA_MEM* GeneralHostControl;
// Arch-specific.
uint32_t HbaIrqNumber;
Port Ports[32];
irq HbaIrq;
pci_device_node PCINode;
bool FoundPCINode;
pci_iteration_decision find_pci_node(void* udata, pci_device_node node)
{
    OBOS_UNUSED(udata);
    if (node.device.indiv.classCode == drv_hdr.pciId.indiv.classCode && 
        node.device.indiv.subClass == drv_hdr.pciId.indiv.subClass &&
        node.device.indiv.progIf == drv_hdr.pciId.indiv.progIf)
    {
        PCINode = node;
        FoundPCINode = true;
        return PCI_ITERATION_DECISION_ABORT;
    }
    return PCI_ITERATION_DECISION_CONTINUE;
}
uacpi_ns_iteration_decision pci_bus_match(void *user, uacpi_namespace_node *node)
{
    uacpi_namespace_node** pNode = (uacpi_namespace_node**)user;
    *pNode = node;
    return UACPI_NS_ITERATION_DECISION_BREAK;
}

void OBOS_DriverEntry()
{
    DrvS_EnumeratePCI(find_pci_node, nullptr);
    if (!FoundPCINode)
    {
        OBOS_Error("%*s: Could not find PCI Node. Aborting...\n", uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName);
        Core_ExitCurrentThread();
    }
    uintptr_t bar = PCINode.bars.indiv32.bar5;
    // for (volatile bool b = true; b; )
    //     ;
    uint64_t pciCommand = 0;
    size_t barlen = DrvS_GetBarSize(PCINode.info, 5, false, nullptr);
    barlen = barlen + (OBOS_PAGE_SIZE - (barlen % OBOS_PAGE_SIZE));
    OBOS_Log("%*s: Initializing AHCI controller at %02x:%02x:%02x. HBA at 0x%p-0x%p.\n",
        uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName,
        PCINode.info.bus, PCINode.info.slot, PCINode.info.function, 
        bar, bar+barlen);
    OBOS_Debug("Enabling bus master and memory space access in PCI command.\n");
    DrvS_ReadPCIRegister(PCINode.info, 1*4+2, 2, &pciCommand);
    pciCommand |= 6; // memory space + bus master
    DrvS_WritePCIRegister(PCINode.info, 1*4+2, 2, pciCommand);
    OBOS_Debug("Mapping HBA memory.\n");
    GeneralHostControl = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, barlen,
        OBOS_PROTECTION_CACHE_DISABLE, VMA_FLAGS_NON_PAGED,
        nullptr, 
        nullptr);
    page what = {.addr=(uintptr_t)GeneralHostControl};
    for (uintptr_t offset = 0; offset < barlen; offset += OBOS_PAGE_SIZE)
    {
        what.addr = (uintptr_t)GeneralHostControl + offset;
        page* page = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        MmS_SetPageMapping(Mm_KernelContext.pt, page, bar + offset);
    }
    OBOS_Debug("Mapped HBA memory at 0x%p-0x%p.\n", GeneralHostControl, ((uintptr_t)GeneralHostControl)+barlen);
    HbaIrqNumber = PCINode.irq.int_pin - 1;
    OBOS_Debug("AHCI Controller is on PIN%c#.\n", HbaIrqNumber + 'A');
    // Get PCI bus.
    uacpi_namespace_node* pciBus = nullptr;
    uacpi_find_devices("PNP0A03", pci_bus_match, &pciBus);
    uacpi_pci_routing_table *pci_routing_table = nullptr;
    uacpi_get_pci_routing_table(pciBus, &pci_routing_table);
    for (size_t i = 0; i < pci_routing_table->num_entries; i++)
    {
        if (pci_routing_table->entries[i].pin != HbaIrqNumber)
            continue;
        if (pci_routing_table->entries[i].source == 0)
            HbaIrqNumber = pci_routing_table->entries[i].index;
        else
        {
            uacpi_resources* resources = nullptr;
            uacpi_get_current_resources(pci_routing_table->entries[i].source, &resources);
            HbaIrqNumber = 
                (resources->entries[pci_routing_table->entries[i].index].type == UACPI_RESOURCE_TYPE_IRQ) ?
                resources->entries[pci_routing_table->entries[i].index].irq.irqs[0] :
                resources->entries[pci_routing_table->entries[i].index].extended_irq.irqs[0];
            uacpi_free_resources(resources);
        }
        break;
    }
    uacpi_free_pci_routing_table(pci_routing_table);
    OBOS_Debug("Resolved IRQ Pin %u to IRQ %u.\n", PCINode.irq.int_pin, HbaIrqNumber);
    obos_status status = Core_IrqObjectInitializeIRQL(&HbaIrq, IRQL_AHCI, true, true);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "%*s: Could not initialize IRQ object with IRQL %d.\nStatus: %d\n", IRQL_AHCI, status);
#if defined(__x86_64__)
    status = Arch_IOAPICMapIRQToVector(HbaIrqNumber, HbaIrq.vector->id+0x20, true, TriggerModeLevelSensitive);
#endif
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "%*s: Could not register IRQ on IRQ number %u.\nStatus: %d\n", HbaIrqNumber, status);

    Core_ExitCurrentThread();
}