/*
 * drivers/generic/ahci/main.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <scheduler/thread.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

#include <locks/spinlock.h>
#include <locks/semaphore.h>

#include <utils/tree.h>

#include <irq/irq.h>
#include <irq/timer.h>

#include <locks/event.h>

#include <allocators/base.h>

#include <locks/wait.h>

#include "command.h"
#include "structs.h"
#include "ahci_irq.h"

#include <vfs/dirent.h>
#include <vfs/vnode.h>

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

volatile HBA_MEM* HBA;
// Arch-specific.
uint32_t HbaIrqNumber;
enum {
    POLARITY_ACTIVE_LOW,
    POLARITY_ACTIVE_HIGH,
} HbaIrqPolarity;
enum {
    TRIGGER_MODE_LEVEL,
    TRIGGER_MODE_EDGE,
} HbaIrqTriggerMode;
Port Ports[32];
size_t PortCount;
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
void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    void* virt = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, size,
        uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
        nullptr, 
        nullptr);
    page what = {.addr=(uintptr_t)virt};
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        what.addr = (uintptr_t)virt + offset;
        page* page = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        page->prot.uc = true;
        MmS_SetPageMapping(Mm_KernelContext.pt, page, phys + offset);
    }
    return virt;
}
uintptr_t HBAAllocate(size_t size, size_t alignment)
{
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    alignment = alignment + (OBOS_PAGE_SIZE - (alignment % OBOS_PAGE_SIZE));
    alignment /= OBOS_PAGE_SIZE;
    size /= OBOS_PAGE_SIZE;
    if (HBA->cap.s64a)
        return Mm_AllocatePhysicalPages(size, alignment, nullptr);
    else
        return Mm_AllocatePhysicalPages32(size, alignment, nullptr);
    OBOS_UNREACHABLE;
}
const char* const DeviceNames[32] = {
    "sda", "sdb", "sdc", "sdd",
    "sde", "sdf", "sdg", "sdh",
    "sdi", "sdj", "sdk", "sdl",
    "sdm", "sdn", "sdo", "sdp",
    "sdq", "sdr", "sds", "sdt",
    "sdu", "sdv", "sdw", "sdx",
    "sdy", "sdz", "sd1", "sd2",
    "sd3", "sd4", "sd5", "sd6",
};
pci_irq_handle PCIIrqHandle;
void OBOS_DriverEntry(driver_id* this)
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
    DrvS_ReadPCIRegister(PCINode.info, 1*4, 2, &pciCommand);
    pciCommand |= 6; // memory space + bus master
    pciCommand &= ~BIT(0); // io space off
    pciCommand &= ~BIT(10);
    OBOS_Debug("PCI Command/Status: 0x%08x.\n", pciCommand);
    DrvS_WritePCIRegister(PCINode.info, 1*4, 4, pciCommand);
    OBOS_Debug("Mapping HBA memory.\n");
    HBA = map_registers(bar, barlen, true);
    OBOS_Debug("Mapped HBA memory at 0x%p-0x%p.\n", HBA, ((uintptr_t)HBA)+barlen);
    obos_status status = Core_IrqObjectInitializeIRQL(&HbaIrq, IRQL_AHCI, true, true);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "%*s: Could not initialize IRQ object with IRQL %d.\nStatus: %d\n", uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName, IRQL_AHCI, status);
    if (!HBA->cap.sam)
    {
        HBA->ghc.ae = true;
        while(!HBA->ghc.ae)
            OBOSS_SpinlockHint();
    }
    if (HBA->cap2 & BIT(0) /* Bios/OS Handoff (BOH) */)
    {
        OBOS_Debug("Performing Bios/OS handoff. This might take a couple seconds.\n");
        do {
            /*
                1. Sets the OS Ownership (BOHC.OOS) bit to ’1’.
                2. This will cause an SMI so that the BIOS can cleanup and pass control if needed.
                3. Spin on the BIOS Ownership (BOHC.BOS) bit, waiting for it to be cleared to ‘0’.
                4. If the BIOS Busy (BOHC.BB) has been set to ‘1’ within 25 milliseconds, then the OS
                driver shall provide the BIOS a minimum of two seconds for finishing outstanding
                commands on the HBA.
                5. If after 25 milliseconds the BIOS Busy (BOHC.BB) bit has not been set to ‘1’, then the OS
                can assume control and perform whatever cleanup is necessary.
            */
            HBA->bohc |= BIT(1); // BOHC.OOS
            while (HBA->bohc & BIT(0) /* BOHC.BOS */)
                OBOSS_SpinlockHint();
            timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(25000  /* 25 ms */);
            while (!(HBA->bohc & BIT(4)) /* BOHC.BB */ && CoreS_GetTimerTick() < deadline)
                OBOSS_SpinlockHint();
            if (!(HBA->bohc & BIT(4)))
                break;
            // It is set...
            // Spin on it for two seconds, otherwise assume control.
            while (HBA->bohc & BIT(4) /* BOHC.BB */)
                OBOSS_SpinlockHint();
        } while(0);
    }
    OBOS_Debug("Resetting HBA....\n");
    // for (uint8_t i = 0; i < 32; i++)
    // {
    //     if (!(HBA->pi & BIT(i)))
    //         continue;
    //     StopCommandEngine(HBA->ports + i);
    // }
    // HBA->ghc.hr = 1;
    // while (HBA->ghc.hr)
    //     OBOSS_SpinlockHint();
    if (!HBA->cap.sam)
    {
        HBA->ghc.ae = true;
        while(!HBA->ghc.ae)
            OBOSS_SpinlockHint();
    }
    for (uint8_t i = 0; i < 32; i++)
    {
        if (!(HBA->pi & BIT(i)))
            continue;
        Port* port = &Ports[i];
        PortCount++;
        port->hbaPort = &HBA->ports[i];
        port->lock = SEMAPHORE_INITIALIZE(32);
        port->works = false;
        port->fisBasePhys = HBAAllocate(OBOS_PAGE_SIZE*1, 256);
        port->clBasePhys = HBAAllocate(OBOS_PAGE_SIZE*7, 1024);
        port->clBase = map_registers(port->clBasePhys, OBOS_PAGE_SIZE*7, true);
        port->fisBase = map_registers(port->fisBasePhys, OBOS_PAGE_SIZE*1, true);
        port->dev_name = DeviceNames[i];
        memzero((void*)port->clBase, OBOS_PAGE_SIZE*7);
        memzero((void*)port->fisBase, OBOS_PAGE_SIZE*1);
        AHCISetAddress(port->clBasePhys, HBA->ports[i].clb);
        AHCISetAddress(port->fisBasePhys, HBA->ports[i].fb);
        for (uint8_t slot = 0; slot < HBA->cap.nsc; slot++)
        {
            HBA_CMD_HEADER* cmdHeader = (HBA_CMD_HEADER*)port->clBase + slot;
			uintptr_t ctba = port->clBasePhys + sizeof(HBA_CMD_HEADER) * 32 + slot * sizeof(HBA_CMD_TBL);
            AHCISetAddress(ctba, cmdHeader->ctba);
        }
        // StartCommandEngine(HBA->ports + i);
        HBA->ports[i].cmd |= BIT(4) /* FRE */;
        HBA->ports[i].cmd |= BIT(0) /* ST */;
        if (HBA->cap.sss)
            HBA->ports[i].cmd |= BIT(1); // Staggered spin-up
        timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(1000000  /* 1S */);
		while ((HBA->ports[i].ssts & 0xf) != HBA_PORT_DET_PRESENT && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
		if ((HBA->ports[i].ssts & 0xf) != HBA_PORT_DET_PRESENT)
		{
			OBOS_Warning("%*s: No drive found on port %d, even though it is implemented.\n", uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName, i);
			continue;
		}
        HBA->ports[i].serr = ~0;
        // Wait for the port.
        // 0x88: ATA_DEV_BUSY | ATA_DEV_DRQ
        while ((port->hbaPort->tfd & 0x88))
            OBOSS_SpinlockHint();
        HBA->ports[i].serr = ~0;
        OBOS_Debug("Found a port at index %d.\n", i);
        port->works = true;
    }
    HBA->ghc.ie = true;
    Drv_RegisterPCIIrq(&HbaIrq, &PCINode, &PCIIrqHandle);
    HbaIrq.handler = ahci_irq_handler;
    HbaIrq.irqChecker = ahci_irq_checker;
    Drv_MaskPCIIrq(&PCIIrqHandle, false);
    for (uint8_t i = 0; i < 32; i++)
    {
        Port* port = &Ports[i];
        if (!port->works)
            continue;
        OBOS_Debug("Resetting port %d.\n", i);
        StopCommandEngine(port->hbaPort);
        HBA->ports[i].cmd |= BIT(4) /* FRE */;
        HBA->ports[i].sctl |= 1 /* DET=INIT */;
        timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(1000  /* 1 ms */);
		while (CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        HBA->ports[i].sctl &= ~0xf /* DET=NONE_REQUESTED */;
        deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(2000000  /* 2S */);
		while ((HBA->ports[i].ssts & 0xf) != HBA_PORT_DET_PRESENT && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        if ((HBA->ports[i].ssts & 0xf) != HBA_PORT_DET_PRESENT)
            OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Port physical layer not online after reset.\n");
        HBA->ports[i].serr = ~0;
        // Wait for the port.
        // 0x88: ATA_DEV_BUSY | ATA_DEV_DRQ
        while ((port->hbaPort->tfd & 0x88))
            OBOSS_SpinlockHint();
        HBA->ports[i].serr = ~0;
        HBA->ports[i].is = HBA->ports[i].is;
        HBA->ports[i].ie = 0xffffffff;
        StartCommandEngine(port->hbaPort);
    }
    for (uint8_t i = 0; i < 32; i++)
    {
        Port* port = &Ports[i];
        if (!port->works)
            continue;
        // Send Identify ATA.
        struct ahci_phys_region reg = {
            .phys = HBAAllocate(4096, 4096),
            .sz = 4096
        };
        struct command_data data = {};
        data.phys_regions = &reg;
        data.physRegionCount = 1;
        data.cmd = ATA_IDENTIFY_DEVICE;
        data.direction = COMMAND_DIRECTION_READ;
        data.completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        size_t tries = 0;
        OBOS_Debug("Sending IDENTIFY_ATA to port %d.\n", i);
        retry:
        HBA->ports[i].is = HBA->ports[i].is;
        HBA->ports[i].ie = 0xffffffff;
        SendCommand(port, &data, 0, 0, 0);
        while (HBA->ports[i].ci & BIT(data.internal.cmdSlot))
            OBOSS_SpinlockHint();
        OBOS_Debug("PxIe: 0x%08x\n", HBA->ports[i].ie);
        OBOS_Debug("PxIs: 0x%08x\n", HBA->ports[i].is);
        OBOS_Debug("GhcIe: 0x%x\n", HBA->ghc.ie);
        Core_WaitOnObject(WAITABLE_OBJECT(data.completionEvent));
        Core_EventClear(&data.completionEvent);
        if (data.commandStatus != OBOS_STATUS_SUCCESS)
        {
            if (tries++ >= 10)
            {
                Mm_FreePhysicalPages(reg.phys, reg.sz/OBOS_PAGE_SIZE);
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)port->clBase, OBOS_PAGE_SIZE);
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)port->fisBase, OBOS_PAGE_SIZE*7);
                continue;
            }
            OBOS_Debug("Command failed. Retrying...\n");
            data.commandStatus = OBOS_STATUS_SUCCESS;
            goto retry;
        }
        void* response = map_registers(reg.phys, reg.sz, false);
        port->sectorSize = *(uint32_t*)(response + (117 * 2));
		if (!port->sectorSize)
			port->sectorSize = 512; // Assume one sector = 512 bytes.
		port->type = port->hbaPort->sig == SATA_SIG_ATA ? DRIVE_TYPE_SATA : DRIVE_TYPE_SATAPI;
		port->nSectors = *(uint64_t*)(response + (100 * 2));
        OBOS_Log("AHCI: Found %s drive at port %s. Sector count: 0x%016X, sector size 0x%08X.\n",
			port->type == DRIVE_TYPE_SATA ? "SATA" : "SATAPI",
			port->dev_name,
			port->nSectors,
			port->sectorSize);
        port->vn = Drv_AllocateVNode(this, (dev_desc)port, port->nSectors*port->sectorSize, nullptr, VNODE_TYPE_BLK);
        Drv_RegisterVNode(port->vn, port->dev_name);
    }
    Core_ExitCurrentThread();
}