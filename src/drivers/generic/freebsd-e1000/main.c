/*
 * drivers/generic/freebsd-1000/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <e1000/e1000_hw.h>

#include <allocators/base.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <net/eth.h>
#include <net/tables.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/irp.h>

#include <locks/event.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>
#include <driver_interface/driverId.h>

#include "dev.h"
#include "e1000/e1000_defines.h"

OBOS_PAGEABLE_FUNCTION obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    OBOS_UNUSED(desc && count);
    return OBOS_STATUS_INVALID_OPERATION;
}
obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    e1000_handle* hnd = (void*)what;
    if (hnd->magic != E1000_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (request) {
        case IOCTL_IFACE_MAC_REQUEST:
        {
            e1000_read_mac_addr(&hnd->dev->hw);
            memcpy(argp, hnd->dev->hw.mac.addr, sizeof(mac_address));
            break;
        }
        default:
            return Net_InterfaceIoctl(hnd->dev->vn, request, argp);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status ioctl_argp_size(uint32_t request, size_t* out)
{
    switch (request) {
        case IOCTL_IFACE_MAC_REQUEST:
        {
            *out = sizeof(mac_address);
            break;
        }
        default:
            return Net_InterfaceIoctlArgpSize(request, out);
    }
    return OBOS_STATUS_SUCCESS;
}
void driver_cleanup_callback()
{
    // TODO: cleanup
}

static void irp_on_rx_event_set(irp* req)
{
    e1000_handle* hnd = (void*)req->desc;
    e1000_device* dev = hnd->dev;
    if (req->evnt)
        Core_EventClear(req->evnt);
    if (!hnd->rx_curr)
        hnd->rx_curr = LIST_GET_TAIL(e1000_frame_list, &hnd->dev->rx_frames);
    if (!hnd->rx_curr)
    {
        req->status = OBOS_STATUS_IRP_RETRY;
        return;
    }
    req->status = OBOS_STATUS_SUCCESS;
    if (req->dryOp)
    {
        if (hnd->rx_curr)
            req->nBlkRead = hnd->rx_curr->size;
        return;
    }

    size_t szRead = OBOS_MIN(req->blkCount, hnd->rx_curr->size - hnd->rx_off);
    memcpy(req->buff, hnd->rx_curr->buff + hnd->rx_off, szRead);
    hnd->rx_off += szRead;
    if (hnd->rx_off >= hnd->rx_curr->size)
    {
        e1000_frame* next = LIST_GET_NEXT(e1000_frame_list, &dev->rx_frames, hnd->rx_curr);
        hnd->last_rx = hnd->rx_curr;
        if (!(--hnd->rx_curr->refs))
        {
            hnd->last_rx = nullptr;
            LIST_REMOVE(e1000_frame_list, &dev->rx_frames, hnd->rx_curr);
            Free(OBOS_NonPagedPoolAllocator, hnd->rx_curr->buff, hnd->rx_curr->size);
            Free(OBOS_NonPagedPoolAllocator, hnd->rx_curr, sizeof(*hnd->rx_curr));
            // hnd->rx_curr = 0;
        }
        hnd->rx_curr = next;
        hnd->rx_off = 0;
    }
    req->nBlkRead = szRead;
}

static void irp_on_tx_event_set(irp* req)
{
    e1000_handle* hnd = (void*)req->desc;
    if (req->evnt)
        Core_EventClear(req->evnt);
    req->status = OBOS_STATUS_SUCCESS;
    req->evnt = e1000_tx_packet(hnd->dev, req->cbuff,req->blkCount, req->dryOp, &req->status);
    req->on_event_set = nullptr;
    if (obos_is_success(req->status))
        req->status = req->evnt ? OBOS_STATUS_IRP_RETRY : OBOS_STATUS_SUCCESS;
    if (req->status)
        req->nBlkWritten = req->blkCount;
}

obos_status submit_irp(void* request)
{
    irp* req = request;
    e1000_handle* hnd = (void*)req->desc;
    if (!hnd || hnd->magic != E1000_HANDLE_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->op == IRP_READ)
    {
        if (!hnd->rx_curr)
            hnd->rx_curr = LIST_GET_HEAD(e1000_frame_list, &hnd->dev->rx_frames);
        if (!hnd->rx_curr || hnd->last_rx == hnd->rx_curr)
        {
            hnd->rx_curr = nullptr;
            req->evnt = &hnd->dev->rx_evnt;
            req->on_event_set = irp_on_rx_event_set;
        }
        else
            irp_on_rx_event_set(req);
    }
    else
    {
        req->status = OBOS_STATUS_SUCCESS;
        req->evnt = e1000_tx_packet(hnd->dev, req->cbuff, req->blkCount, req->dryOp, &req->status);
        req->on_event_set = irp_on_tx_event_set;
        if (obos_is_success(req->status))
            req->nBlkWritten = req->blkCount;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status finalize_irp(void* request)
{
    irp* req = request;
    if (!req) return OBOS_STATUS_INVALID_ARGUMENT;
    // if (req->evnt)
    //     Core_EventClear(req->evnt);
    return OBOS_STATUS_SUCCESS;
}

obos_status reference_device(dev_desc *pdesc)
{
    if (!pdesc || !*pdesc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    e1000_device* dev = (void*)*pdesc;
    e1000_handle* hnd = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(e1000_handle),nullptr);
    hnd->rx_curr = LIST_GET_TAIL(e1000_frame_list, &dev->rx_frames);
    hnd->rx_off = 0;
    hnd->dev = dev;
    hnd->magic = E1000_HANDLE_MAGIC;
    dev->refs++;
    *pdesc = (dev_desc)hnd;
    return OBOS_STATUS_SUCCESS;
}
obos_status unreference_device(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    e1000_handle* hnd = (void*)desc;
    hnd->dev->refs--;
    Free(OBOS_NonPagedPoolAllocator, hnd, sizeof(*hnd));
    return OBOS_STATUS_SUCCESS;
}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES |
             DRIVER_HEADER_FLAGS_DETECT_VIA_PCI |
             DRIVER_HEADER_PCI_IGNORE_PROG_IF | 
             DRIVER_HEADER_HAS_VERSION_FIELD,
    .pciId = {
        .indiv = {
            .classCode = 0x02  , // Network Controller
            .subClass  = 0x00  , // Ethernet Controller
            .progIf    = 0x00  , // Ignored
        }
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .ioctl_argp_size = ioctl_argp_size,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = nullptr,
        .foreach_device = nullptr,
        .read_sync = nullptr,
        .write_sync = nullptr,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,
        .reference_device = reference_device,
        .unreference_device = unreference_device,
    },
    .driverName = "E1000 Driver",
    .version = 1,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

#include "device-ids.h"

#include <irq/timer.h>

void e1000_sleep_us(uint64_t us)
{
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(us);
    while (deadline > CoreS_GetTimerTick())
        OBOSS_SpinlockHint();
}

static driver_id* this_driver;

e1000_device *Devices = nullptr;
size_t nDevices = 0, nInitializedDevices = 0;

static void* map_registers(uintptr_t phys, size_t size, bool uc, bool mmio, bool ref_twice)
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
        do {
            struct page what = {.phys=page.phys};
            Core_MutexAcquire(&Mm_PhysicalPagesLock);
            struct page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            Core_MutexRelease(&Mm_PhysicalPagesLock);
            MmH_DerefPage(pg);
        } while(0);
        page.prot.uc = uc;
        page.phys = phys+offset;
        struct page* pg = MmH_AllocatePage(page.phys, false);
        if (mmio)
            pg->flags |= PHYS_PAGE_MMIO;
        if (ref_twice)
            MmH_RefPage(pg);
        pg->pagedCount++;
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    Drv_TLBShootdown(Mm_KernelContext.pt, (uintptr_t)virt, size);
    return virt+phys_page_offset;
}

static void init_e1000(pci_bus* bus, pci_device* dev, size_t dev_index)
{
    pci_resource* bar0 = nullptr;
    pci_resource* io_bar = nullptr;
    pci_resource* irq_res = nullptr;
    for (pci_resource* res = LIST_GET_HEAD(pci_resource_list, &dev->resources); res; )
    {
        if (res->type == PCI_RESOURCE_BAR && res->bar->idx == 0)
            bar0 = res;
        if (res->type == PCI_RESOURCE_BAR && res->bar->type == PCI_BARIO)
            io_bar = res;
        if (res->type == PCI_RESOURCE_IRQ)
            irq_res = res;

        if (bar0 && irq_res && io_bar)
            break;

        res = LIST_GET_NEXT(pci_resource_list, &dev->resources, res);
    }

    if (!bar0 || !irq_res)
    {
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }
    
    // for (volatile bool b = true; b;)
    //     OBOSS_SpinlockHint();

    dev->resource_cmd_register->cmd_register |= 0x7;
    Drv_PCISetResource(dev->resource_cmd_register);
    
    Devices[dev_index].hw.back = &Devices[dev_index].osdep;
    Devices[dev_index].osdep.pci = dev;
    Devices[dev_index].osdep.iobase = io_bar ? io_bar->bar->iospace : 0;
    Devices[dev_index].osdep.membase = (uintptr_t)map_registers(bar0->bar->phys, bar0->bar->size, true, true, false);
    Devices[dev_index].hw.io_base = Devices[dev_index].osdep.iobase;
    Devices[dev_index].hw.hw_addr = (void*)Devices[dev_index].osdep.membase;
    Devices[dev_index].hw.vendor_id = dev->hid.indiv.vendorId;
    Devices[dev_index].hw.device_id = dev->hid.indiv.deviceId;
    Devices[dev_index].hw.revision_id = 0;
    e1000_read_pci_cfg(&Devices[dev_index].hw, 0x2c, &Devices[dev_index].hw.subsystem_vendor_id);
    e1000_read_pci_cfg(&Devices[dev_index].hw, 0x2e, &Devices[dev_index].hw.subsystem_device_id);
    if (e1000_set_mac_type(&Devices[dev_index].hw) != E1000_SUCCESS)
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }
    if (!io_bar && (Devices[dev_index].hw.mac.type < e1000_82547 && Devices[dev_index].hw.mac.type > e1000_82543))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }

    // Taken from managarm
    if((Devices[dev_index].hw.mac.type == e1000_ich8lan) || (Devices[dev_index].hw.mac.type == e1000_ich9lan) ||
        (Devices[dev_index].hw.mac.type == e1000_ich10lan) || (Devices[dev_index].hw.mac.type == e1000_pchlan) ||
        (Devices[dev_index].hw.mac.type == e1000_pch2lan) || (Devices[dev_index].hw.mac.type == e1000_pch_lpt))
    {
        OBOS_Warning("%02x:%02x:%02x: e1000: Mapping of flash unimplemented\n", dev->location.bus, dev->location.slot, dev->location.function);
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    } else if (Devices[dev_index].hw.mac.type >= e1000_pch_spt) {
        /**
        * In the new SPT device flash is not a separate BAR, rather it is also in BAR0,
        * so use the same tag and an offset handle for the FLASH read/write macros in the shared code.
        */

        hw2flashbase(&Devices[dev_index].hw) = Devices[dev_index].osdep.membase + E1000_FLASH_BASE_ADDR;
    }
    Devices[dev_index].hw.flash_address = (void*)Devices[dev_index].osdep.flashbase;

    if (e1000_setup_init_funcs(&Devices[dev_index].hw, true) != E1000_SUCCESS)
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }

    e1000_get_bus_info(&Devices[dev_index].hw);

    Devices[dev_index].hw.mac.autoneg = 1;
    Devices[dev_index].hw.phy.autoneg_wait_to_complete = false;
    Devices[dev_index].hw.phy.autoneg_advertised = (ADVERTISE_10_HALF | ADVERTISE_10_FULL | ADVERTISE_100_HALF | ADVERTISE_100_FULL | ADVERTISE_1000_FULL);

    if (Devices[dev_index].hw.phy.media_type == e1000_media_type_copper) {
        Devices[dev_index].hw.phy.mdix = 0;
        Devices[dev_index].hw.phy.disable_polarity_correction = false;
        Devices[dev_index].hw.phy.ms_type = e1000_ms_hw_default;
    }

    Devices[dev_index].hw.mac.report_tx_early = true;

    if (e1000_reset_hw(&Devices[dev_index].hw) != E1000_SUCCESS)
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }
    e1000_power_up_phy(&Devices[dev_index].hw);

    if (e1000_validate_nvm_checksum(&Devices[dev_index].hw) != E1000_SUCCESS)
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[dev_index].osdep.membase, bar0->bar->size);
        nDevices--;
        Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
        OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
        return;
    }

    e1000_disable_ulp_lpt_lp(&Devices[dev_index].hw, true);

    Devices[dev_index].rx_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    Devices[dev_index].tx_done_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    Devices[dev_index].irq_res = irq_res;

    e1000_init_rx(&Devices[dev_index]);
    e1000_init_tx(&Devices[dev_index]);

    Core_IrqObjectInitializeIRQL(&Devices[dev_index].irq, IRQL_E1000, true, true);
    Devices[dev_index].irq.handler = e1000_irq_handler;
    Devices[dev_index].irq.irqChecker = e1000_check_irq_callback;
    Devices[dev_index].irq.irqCheckerUserdata = Devices + dev_index;
    Devices[dev_index].irq.handlerUserdata = Devices + dev_index;
    Devices[dev_index].irq_res->irq->irq = &Devices[dev_index].irq;
    Devices[dev_index].irq_res->irq->masked = false;
    Drv_PCISetResource(Devices[dev_index].irq_res);
    Devices[dev_index].irq.handler = e1000_irq_handler;
    Devices[dev_index].irq.irqChecker = e1000_check_irq_callback;
    Devices[dev_index].irq.irqCheckerUserdata = Devices + dev_index;
    Devices[dev_index].irq.handlerUserdata = Devices + dev_index;

    e1000_clear_hw_cntrs_base_generic(&Devices[dev_index].hw);
    E1000_WRITE_REG(&Devices[dev_index].hw, E1000_IMS, IMS_ENABLE_MASK);
}
static void search_bus(pci_bus* bus, bool enumerate)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev; )
    {
        if ((dev->hid.id & 0xffffffff) == (drv_hdr.pciId.id & 0xffffffff))
        {
            // Compare Device IDs.
            bool found = false;
            for (size_t i = 0; i < sizeof(device_ids)/sizeof(device_ids[0]); i++)
            {
                if (dev->hid.indiv.deviceId == device_ids[i])
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }
            if (enumerate)
                nDevices++;
            else
                init_e1000(bus, dev, nInitializedDevices++);
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (uint16_t bus = 0; bus < Drv_PCIBusCount; bus++)
        search_bus(&Drv_PCIBuses[bus], true);
    Devices = ZeroAllocate(OBOS_KernelAllocator, nDevices, sizeof(*Devices), nullptr);
    nInitializedDevices = 0;
    for (uint16_t bus = 0; bus < Drv_PCIBusCount; bus++)
        search_bus(&Drv_PCIBuses[bus], false);
    for (size_t i = 0; i < nDevices; i++)
    {
        size_t sz_name = snprintf(nullptr, 0, "e1000-eth%lu", i);
        Devices[i].interface_name = Vfs_Malloc(sz_name+1);
        snprintf(Devices[i].interface_name, sz_name+1, "e1000-eth%lu", i);
        Devices[i].vn = Drv_AllocateVNode(this, (dev_desc)&Devices[i], 0, nullptr, VNODE_TYPE_CHR);
        Devices[i].vn->flags |= VFLAGS_NIC_NO_FCS;
        Drv_RegisterVNode(Devices[i].vn, Devices[i].interface_name);
    }
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false};
}

void e1000_pci_clear_mwi(struct e1000_hw *hw)
{
    OBOS_UNUSED(hw);
    OBOS_Debug("%s unimplemented\n", __func__);
}
void e1000_pci_set_mwi(struct e1000_hw *hw)
{
    OBOS_UNUSED(hw);
    OBOS_Debug("%s unimplemented\n", __func__);
}

#define PCIY_EXPRESS 0x10
s32 e1000_read_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
    pci_capability* cached_cap = hw2pcicap(hw);
    if (!cached_cap)
    {
        for (pci_capability* cap = hw2pci(hw)->first_capability; cap; cap = cap->next_cap)
        {
            if (cap->id == PCIY_EXPRESS)
            {
                cached_cap = cap;
                break;
            }
        }
        hw2pcicap(hw) = cached_cap;        
    }
    uint64_t res = 0;
    *value = DrvS_ReadPCIRegister(hw2pci(hw)->location, cached_cap->offset+reg, 2, &res);
    *value = res;
    return E1000_SUCCESS;
}

s32 e1000_write_pcie_cap_reg(struct e1000_hw *hw, u32 reg, u16 *value)
{
    pci_capability* cached_cap = hw2pcicap(hw);
    if (!cached_cap)
    {
        for (pci_capability* cap = hw2pci(hw)->first_capability; cap; cap = cap->next_cap)
        {
            if (cap->id == PCIY_EXPRESS)
            {
                cached_cap = cap;
                break;
            }
        }
        hw2pcicap(hw) = cached_cap;        
    }
    *value = DrvS_WritePCIRegister(hw2pci(hw)->location, cached_cap->offset+reg, 2, *value);
    return E1000_SUCCESS;
}

void e1000_read_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
    pci_device* dev = hw2pci(hw);
    uint64_t val = 0;
    DrvS_ReadPCIRegister(dev->location, reg, 2, &val);
    *value = val;
}

void e1000_write_pci_cfg(struct e1000_hw *hw, u32 reg, u16 *value)
{
    pci_device* dev = hw2pci(hw);
    DrvS_WritePCIRegister(dev->location, reg, 2, *value);
}

void e1000_io_write(struct e1000_hw *hw, u16 reg, u32 data)
{
    pci_bar fake_bar = {
        .idx = 0,
        .iospace = hw2iobase(hw),
        .size = reg+4,
        .type = PCI_BARIO,
    };
    DrvS_WriteIOSpaceBar(&fake_bar, reg, data, 4);
}