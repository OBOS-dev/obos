/*
 * drivers/generic/ahci/main.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <scheduler/thread.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>
#include <driver_interface/driverId.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

#include <locks/spinlock.h>
#include <locks/semaphore.h>

#include <utils/tree.h>

#include <irq/irq.h>
#include <irq/timer.h>
#include <irq/irql.h>

#include <allocators/base.h>

#include "command.h"
#include "ahci_irq.h"
#include "mm/page.h"
#include "structs.h"

#include <uacpi/namespace.h>
#include <uacpi/resources.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <uacpi_libc.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <utils/list.h>

#if defined(__x86_64__)
#include <arch/x86_64/ioapic.h>
#endif

pci_resource* PCIIrqResource = nullptr;

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_WEAK obos_status submit_irp(void*);
OBOS_PAGEABLE_FUNCTION obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}
void driver_cleanup_callback()
{
    HaltTranscations();
    WaitForTranscations();
    if (PCIIrqResource)
    {
        PCIIrqResource->irq->masked = true;
        Drv_PCISetResource(PCIIrqResource);
    }
    Core_IrqObjectFree(&HbaIrq);
    // TODO: Free HBA, port clb, and fb
    
}

driver_id* this_driver;

void on_wake();
void on_suspend();

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_FLAGS_DETECT_VIA_PCI|DRIVER_HEADER_HAS_VERSION_FIELD,
    .acpiId.nPnpIds = 0,
    .pciId.indiv = {
        .classCode = 0x01, // mass storage controller
        .subClass  = 0x06, // SATA Controller
        .progIf    = 0x01, // AHCI 1.0
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_wake = on_wake,
        .on_suspend = on_suspend,
        .submit_irp = submit_irp,
    },
    .driverName = "AHCI Driver",
    .version=1,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

volatile HBA_MEM* HBA;
// Arch-specific.
uint32_t HbaIrqNumber;
Port Ports[32];
size_t PortCount;
// TODO: Make AHCI driver support multiple controllers.
pci_device* PCINode;
bool FoundPCINode;

static void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size_t phys_page_offset = (phys % OBOS_PAGE_SIZE);
    phys -= phys_page_offset;
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    size += phys_page_offset;
    void* virt = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, size,
        uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
        nullptr, 
        nullptr);
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        page_info page = {.virt=offset+(uintptr_t)virt};
        MmS_QueryPageInfo(Mm_KernelContext.pt, page.virt, &page, nullptr);
        page.prot.uc = uc;
        page.phys = phys+offset;
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    return virt+phys_page_offset;
}

uintptr_t HBAAllocate(size_t size, size_t alignment)
{
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    alignment = alignment + (OBOS_PAGE_SIZE - (alignment % OBOS_PAGE_SIZE));
    alignment /= OBOS_PAGE_SIZE;
    size /= OBOS_PAGE_SIZE;
    if (HBA->cap & BIT(31))
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

static void search_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        if ((dev->hid.id & 0xffffffff) == (drv_hdr.pciId.id & 0xffffffff))
        {
            PCINode = dev;
            FoundPCINode = true;
            break;
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}
// https://forum.osdev.org/viewtopic.php?t=40969
driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (size_t i = 0; i < Drv_PCIBusCount; i++)
        search_bus(&Drv_PCIBuses[i]);
    if (!FoundPCINode)
        return (driver_init_status){.status=OBOS_STATUS_NOT_FOUND,.fatal=true,.context="Could not find PCI Device."};
    //uintptr_t bar = PCINode.bars.indiv32.bar5;
    // Look for BAR 5.
    pci_resource* bar = nullptr;
    for (pci_resource* curr = LIST_GET_HEAD(pci_resource_list, &PCINode->resources); curr; )
    {
        if (curr->type == PCI_RESOURCE_BAR)
        {
            if (curr->bar->idx == 5)
                bar = curr;
        } else if (curr->type == PCI_RESOURCE_IRQ)
            PCIIrqResource = curr;

        if (bar && PCIIrqResource)
            break;

        curr = LIST_GET_NEXT(pci_resource_list, &PCINode->resources, curr);
    }

    size_t barlen = bar->bar->size;
    OBOS_Log("%*s: Initializing AHCI controller at %02x:%02x:%02x. HBA at 0x%p-0x%p.\n",
        uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName,
        PCINode->location.bus, PCINode->location.slot, PCINode->location.function,
        bar, bar+barlen);

    OBOS_Debug("Enabling bus master and memory space access in PCI command.\n");
    PCINode->resource_cmd_register->cmd_register |= 6; // memory space + bus master
    Drv_PCISetResource(PCINode->resource_cmd_register);
    OBOS_Debug("Mapping HBA memory.\n");
    HBA = map_registers(bar->bar->phys, barlen, true);
    // OBOS_Log("Mapped HBA memory at 0x%p-0x%p.\n", HBA, ((uintptr_t)HBA)+barlen);
    obos_status status = Core_IrqObjectInitializeIRQL(&HbaIrq, IRQL_AHCI, true, true);
    if (obos_is_error(status))
        return (driver_init_status){.status=status,.fatal=true,.context="Could not initialize IRQ object."};
    OBOS_Debug("Enabling IRQs...\n");
/*    status = Drv_RegisterPCIIrq(&HbaIrq, &PCINode, &PCIIrqHandle);
    if (obos_is_error(status))
        return (driver_init_status){.status=status,.fatal=true,.context="Could not initialize HBA Irq."};
    status = Drv_MaskPCIIrq(&PCIIrqHandle, false);
    if (obos_is_error(status))
        return (driver_init_status){.status=status,.fatal=true,.context="Could not unmask HBA Irq."};*/
    PCIIrqResource->irq->irq = &HbaIrq;
    PCIIrqResource->irq->masked = false;
    Drv_PCISetResource(PCIIrqResource);
    HbaIrq.irqChecker = ahci_irq_checker;
    HbaIrq.handler = ahci_irq_handler;
    OBOS_Debug("Enabled IRQs.\n");
    HBA->ghc |= BIT(31);
    while (!(HBA->ghc & BIT(31)))
        OBOSS_SpinlockHint();
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
            deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(2*1000000  /* 2s */);
            while (HBA->bohc & BIT(4) /* BOHC.BB */ && CoreS_GetTimerTick() < deadline)
                OBOSS_SpinlockHint();
        } while(0);
    }
    for (uint8_t i = 0; i < 32; i++)
    {
        if (!(HBA->pi & BIT(i)))
            continue;
        HBA_PORT* hPort = HBA->ports + i;
        // StopCommandEngine(hPort);
        hPort->cmd &= ~(1<<0);
        while(hPort->cmd & (1<<15) /*PxCMD.CR*/)
            OBOSS_SpinlockHint();
        hPort->cmd &= ~(1<<4);
        while(hPort->cmd & (1<<14) /*PxCMD.FR*/)
            OBOSS_SpinlockHint();
    }
    HBA->ghc |= BIT(0);
    while (HBA->ghc & BIT(0))
        OBOSS_SpinlockHint();
    HBA->ghc |= BIT(31);
    while (!(HBA->ghc & BIT(31)))
        OBOSS_SpinlockHint();
    HBA->ghc |= BIT(1);
    for (uint8_t port = 0; port < 32; port++)
    {
        if (!(HBA->pi & BIT(port)))
            continue;
        HBA_PORT* hPort = HBA->ports + port;
        Port* curr = &Ports[PortCount++];
        curr->clBasePhys = HBAAllocate(sizeof(HBA_CMD_HEADER)*32+sizeof(HBA_CMD_TBL)*32, 0);
        curr->clBase = map_registers(curr->clBasePhys, sizeof(HBA_CMD_HEADER)*32+sizeof(HBA_CMD_TBL)*32, true);
        curr->fisBasePhys = HBAAllocate(4096, 0);
        curr->fisBase = map_registers(curr->fisBasePhys, 4096, true);
        memzero((void*)curr->fisBase, 4096);
        memzero((void*)curr->clBase, sizeof(HBA_CMD_HEADER)*32+sizeof(HBA_CMD_TBL)*32);
        for (uint8_t slot = 0; slot < (((HBA->cap >> 8) & 0b11111)+1); slot++)
        {
            HBA_CMD_HEADER* cmdHeader = (HBA_CMD_HEADER*)curr->clBase + slot;
			uintptr_t ctba = curr->clBasePhys + sizeof(HBA_CMD_HEADER) * 32 + slot * sizeof(HBA_CMD_TBL);
            AHCISetAddress(ctba, cmdHeader->ctba);
        }
        AHCISetAddress(curr->clBasePhys, hPort->clb);
        AHCISetAddress(curr->fisBasePhys, hPort->fb);
        hPort->cmd |= BIT(4);
        if (HBA->cap & BIT(27) /* CapSSS */)
            hPort->cmd |= BIT(1);
        timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(1000);
        while ((hPort->ssts & 0xf) != 0x3 && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        curr->hbaPortIndex = port;
        if ((hPort->ssts & 0xf) != 0x3)
            continue;
        hPort->serr = 0xffffffff;
        deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(10000);
        while (hPort->tfd & 0x80 && hPort->tfd & 0x8 && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        if ((hPort->tfd & 0x88) != 0)
            continue;
        OBOS_Debug("Done port init for port %d.\n", port);
        StartCommandEngine(hPort);
        curr->works = true;
    }
    OBOS_Log("%*s: Initialized %d ports.\n", uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName, PortCount);
    for (uint8_t i = 0; i < PortCount; i++)
    {
        Port* port = Ports + i;
        if (!port->works)
            continue;
        OBOS_Log("%*s: Sending IDENTIFY_ATA to port %d.\n",  uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName, i);
        HBA->ports[port->hbaPortIndex].is = 0xffffffff;
        HBA->ports[port->hbaPortIndex].ie = 0xffffffff;
        HBA->ports[port->hbaPortIndex].serr = 0xffffffff;
        // Send Identify ATA.
        struct ahci_phys_region reg = {
            .phys = HBAAllocate(512, 0),
            .sz = 512
        };
        void* response = map_registers(reg.phys, reg.sz, false);
        memzero(response, reg.sz);
        uint16_t* res_data = (uint16_t*)response;
        struct command_data data = {};
        data.phys_regions = &reg;
        data.physRegionCount = 1;
        data.cmd = ATA_IDENTIFY_DEVICE;
        data.direction = COMMAND_DIRECTION_READ;
        data.completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        port->dev_name = DeviceNames[i];
        port->lock = SEMAPHORE_INITIALIZE((((HBA->cap >> 8) & 0b11111)+1));
        size_t tries = 0;
        retry:
        HBA->ghc &= ~BIT(1);
        // irql oldIrql = Core_RaiseIrql(IRQL_AHCI);
        SendCommand(port, &data, 0, 0, 0);
        // Core_LowerIrql(oldIrql);
        while (!(HBA->ports[port->hbaPortIndex].is))
            OBOSS_SpinlockHint();
        HBA->ghc |= BIT(1);
        Core_EventClear(&data.completionEvent);
        port->type = HBA->ports[port->hbaPortIndex].sig == SATA_SIG_ATA ? DRIVE_TYPE_SATA : DRIVE_TYPE_SATAPI;
        if (port->type == DRIVE_TYPE_SATAPI)
        {
            OBOS_Log("%*s: Cannot send IDENTIFY_ATA to a SATAPI port.\n",  uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName);
            ClearCommand(port, &data);
            continue;
        }
        if (data.commandStatus != OBOS_STATUS_SUCCESS)
        {
            if (tries++ >= 3)
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
        ClearCommand(port, &data);
        uint16_t cmd_feature_set = res_data[83];
        uint16_t cmd_feature_set2 = res_data[86];
        if ((cmd_feature_set & BIT(14)) && !(cmd_feature_set & BIT(15)))
            port->supports48bitLBA = cmd_feature_set & BIT(10);
        if (!port->supports48bitLBA)
        {
            if ((res_data[87] & BIT(14)) && !(res_data[87] & BIT(15)))
            {
                cmd_feature_set = cmd_feature_set2;
                port->supports48bitLBA = cmd_feature_set & BIT(10);
            }
        }
        if (port->supports48bitLBA)
            port->nSectors = *(uint64_t*)(&res_data[100]);
        else
            port->nSectors = *(uint32_t*)(&res_data[60]);
        uint16_t sectorInfo = res_data[106];
        port->sectorSize = 512; // Assume one sector = 512 bytes.
        if ((sectorInfo & BIT(14)) && !(sectorInfo & BIT(15)))
            if (sectorInfo & BIT(12))
                port->sectorSize = *((uint32_t*)&res_data[117]);
        OBOS_Log("AHCI: Found %s drive at port %s. Sector count: 0x%016X, sector size 0x%08X.\n",
			port->type == DRIVE_TYPE_SATA ? "SATA" : "SATAPI",
			port->dev_name,
			port->nSectors,
			port->sectorSize);
        port->vn = Drv_AllocateVNode(this, (dev_desc)port, port->nSectors*port->sectorSize, nullptr, VNODE_TYPE_BLK);
        Drv_RegisterVNode(port->vn, port->dev_name);
    }
    OBOS_Log("%*s: Finished initialization of the HBA.\n", uacpi_strnlen(drv_hdr.driverName, 64), drv_hdr.driverName);
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}

void on_wake()
{
    HbaIrq.irqChecker = ahci_irq_checker;
    HbaIrq.handler = ahci_irq_handler;

    // Set GHC.AE (AHCI Enable)
    // Set GHC.HR (HBA Reset)
    // Set GHC.AE (AHCI Enable) again

    HBA->ghc |= BIT(31);
    while (!(HBA->ghc & BIT(31)))
        OBOSS_SpinlockHint();
    HBA->ghc |= BIT(0);
    while (HBA->ghc & BIT(0))
        OBOSS_SpinlockHint();
    HBA->ghc |= BIT(31);
    while (!(HBA->ghc & BIT(31)))
        OBOSS_SpinlockHint();

    Port* curr = &Ports[0];
    for (uint8_t port = 0; port < 32; port++, curr++)
    {
        HBA_PORT* hPort = HBA->ports + port;
        if (!curr->works)
            continue;
        curr->works = false;
        memzero((void*)curr->fisBase, 4096);
        memzero((void*)curr->clBase, sizeof(HBA_CMD_HEADER)*32+sizeof(HBA_CMD_TBL)*32);
        uint32_t max_slots = (((HBA->cap >> 8) & 0b11111)+1);
        for (uint8_t slot = 0; slot < max_slots; slot++)
        {
            HBA_CMD_HEADER* cmdHeader = (HBA_CMD_HEADER*)curr->clBase + slot;
            uintptr_t ctba = curr->clBasePhys + sizeof(HBA_CMD_HEADER) * 32 + slot * sizeof(HBA_CMD_TBL);
            AHCISetAddress(ctba, cmdHeader->ctba);
        }
        AHCISetAddress(curr->clBasePhys, hPort->clb);
        AHCISetAddress(curr->fisBasePhys, hPort->fb);
        hPort->cmd |= BIT(4);
        if (HBA->cap & BIT(27) /* CapSSS */)
            hPort->cmd |= BIT(1);
        timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(1000);
        while ((hPort->ssts & 0xf) != 0x3 && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        curr->hbaPortIndex = port;
        if ((hPort->ssts & 0xf) != 0x3)
            continue;
        hPort->serr = 0xffffffff;
        deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(10000);
        while (hPort->tfd & 0x80 && hPort->tfd & 0x8 && CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint();
        if ((hPort->tfd & 0x88) != 0)
            continue;
        hPort->is = 0xffffffff;
        hPort->ie = 0xffffffff;
        hPort->serr = 0xffffffff;
        StartCommandEngine(hPort);
        curr->works = true;
    }

    HBA->ghc |= BIT(1);
    while (!(HBA->ghc & BIT(1)))
        OBOSS_SpinlockHint();

    ResumeTranscations();
}
void on_suspend()
{
    HaltTranscations();
    WaitForTranscations();
    HBA->ghc &= ~BIT(1); // IE (interrupt enable)
}

// hexdump clone lol
// static OBOS_NO_KASAN void hexdump(void* _buff, size_t nBytes, const size_t width)
// {
// 	bool printCh = false;
// 	uint8_t* buff = (uint8_t*)_buff;
// 	printf("         Address: ");
// 	for(uint8_t i = 0; i < ((uint8_t)width) + 1; i++)
// 		printf("%02x ", i);
// 	printf("\n%016lx: ", buff);
// 	for (size_t i = 0, chI = 0; i < nBytes; i++, chI++)
// 	{
// 		if (printCh)
// 		{
// 			char ch = buff[i];
// 			switch (ch)
// 			{
// 			case '\n':
// 			case '\t':
// 			case '\r':
// 			case '\b':
// 			case '\a':
// 			case '\0':
// 			{
// 				ch = '.';
// 				break;
// 			}
// 			default:
// 				break;
// 			}
// 			printf("%c", ch);
// 		}
// 		else
// 			printf("%02x ", buff[i]);
// 		if (chI == (size_t)(width + (!(i < (width + 1)) || printCh)))
// 		{
// 			chI = 0;
// 			if (!printCh)
// 				i -= (width + 1);
// 			else
// 				printf(" |\n%016lx: ", &buff[i + 1]);
// 			printCh = !printCh;
// 			if (printCh)
// 				printf("\t| ");
// 		}
// 	}
// }
