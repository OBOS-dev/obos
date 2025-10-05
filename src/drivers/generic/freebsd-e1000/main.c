/*
 * drivers/generic/freebsd-1000/main.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

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
    req->evnt = e1000_tx_packet(hnd->dev, req->cbuff,req->blkCount, req->dryOp);
    req->on_event_set = nullptr;
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
        req->evnt = e1000_tx_packet(hnd->dev, req->cbuff, req->blkCount, req->dryOp);
        req->on_event_set = irp_on_tx_event_set;
        req->status = req->evnt ? OBOS_STATUS_IRP_RETRY : OBOS_STATUS_SUCCESS;
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
size_t nDevices = 0;

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
            struct page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            MmH_DerefPage(pg);
        } while(0);
        page.prot.uc = uc;
        page.phys = phys+offset;
        struct page* pg = MmH_AllocatePage(page.phys, false);
        if (mmio)
            pg->flags |= PHYS_PAGE_MMIO;
        if (ref_twice)
            MmH_RefPage(pg);
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    return virt+phys_page_offset;
}

static void search_bus(pci_bus* bus)
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
                continue;
            }

            nDevices++;
            Devices = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices-1)*sizeof(e1000_device), nullptr);
            memzero(&Devices[nDevices-1], sizeof(Devices[nDevices-1]));

            dev->resource_cmd_register->cmd_register |= 0x7; // io space + memspace + bus master
            Drv_PCISetResource(dev->resource_cmd_register);
            
            Devices[nDevices-1].hw.back = &Devices[nDevices-1].osdep;
            Devices[nDevices-1].osdep.pci = dev;
            Devices[nDevices-1].osdep.iobase = io_bar ? io_bar->bar->iospace : 0;
            Devices[nDevices-1].osdep.membase = (uintptr_t)map_registers(bar0->bar->phys, bar0->bar->size, true, true, false);
            Devices[nDevices-1].hw.io_base = Devices[nDevices-1].osdep.iobase;
            Devices[nDevices-1].hw.hw_addr = (void*)Devices[nDevices-1].osdep.membase;
            Devices[nDevices-1].hw.vendor_id = dev->hid.indiv.vendorId;
            Devices[nDevices-1].hw.device_id = dev->hid.indiv.deviceId;
            Devices[nDevices-1].hw.revision_id = 0;
            e1000_read_pci_cfg(&Devices[nDevices-1].hw, 0x2c, &Devices[nDevices-1].hw.subsystem_vendor_id);
            e1000_read_pci_cfg(&Devices[nDevices-1].hw, 0x2e, &Devices[nDevices-1].hw.subsystem_device_id);
            if (e1000_set_mac_type(&Devices[nDevices-1].hw) != E1000_SUCCESS)
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
                nDevices--;
                Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
                OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }
            if (!io_bar && (Devices[nDevices-1].hw.mac.type < e1000_82547 && Devices[nDevices-1].hw.mac.type > e1000_82543))
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
                nDevices--;
                Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
                OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            // Taken from managarm
            if((Devices[nDevices-1].hw.mac.type == e1000_ich8lan) || (Devices[nDevices-1].hw.mac.type == e1000_ich9lan) ||
                (Devices[nDevices-1].hw.mac.type == e1000_ich10lan) || (Devices[nDevices-1].hw.mac.type == e1000_pchlan) ||
                (Devices[nDevices-1].hw.mac.type == e1000_pch2lan) || (Devices[nDevices-1].hw.mac.type == e1000_pch_lpt))
            {
                OBOS_Warning("%02x:%02x:%02x: e1000: Mapping of flash unimplemented\n", dev->location.bus, dev->location.slot, dev->location.function);
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
                nDevices--;
                Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
                OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            } else if (Devices[nDevices-1].hw.mac.type >= e1000_pch_spt) {
                /**
                * In the new SPT device flash is not a separate BAR, rather it is also in BAR0,
                * so use the same tag and an offset handle for the FLASH read/write macros in the shared code.
                */

                hw2flashbase(&Devices[nDevices-1].hw) = Devices[nDevices-1].osdep.membase + E1000_FLASH_BASE_ADDR;
            }
            Devices[nDevices-1].hw.flash_address = (void*)Devices[nDevices-1].osdep.flashbase;

            if (e1000_setup_init_funcs(&Devices[nDevices-1].hw, true) != E1000_SUCCESS)
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
                nDevices--;
                Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
                OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            e1000_get_bus_info(&Devices[nDevices-1].hw);

            if (e1000_reset_hw(&Devices[nDevices-1].hw) != E1000_SUCCESS)
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
                nDevices--;
                Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
                OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
                continue;
            }

            Devices[nDevices-1].hw.mac.autoneg = 1;
            Devices[nDevices-1].hw.phy.autoneg_wait_to_complete = false;
            Devices[nDevices-1].hw.phy.autoneg_advertised = (ADVERTISE_10_HALF | ADVERTISE_10_FULL | ADVERTISE_100_HALF | ADVERTISE_100_FULL | ADVERTISE_1000_FULL);

            if (Devices[nDevices-1].hw.phy.media_type == e1000_media_type_copper) {
                Devices[nDevices-1].hw.phy.mdix = 0;
                Devices[nDevices-1].hw.phy.disable_polarity_correction = false;
                Devices[nDevices-1].hw.phy.ms_type = e1000_ms_hw_default;
            }

            Devices[nDevices-1].hw.mac.report_tx_early = true;
            
            // if (e1000_init_hw(&Devices[nDevices-1].hw) != E1000_SUCCESS)
            // {
            //     Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)Devices[nDevices-1].osdep.membase, bar0->bar->size);
            //     nDevices--;
            //     Devices = Reallocate(OBOS_NonPagedPoolAllocator, Devices, nDevices*sizeof(e1000_device), (nDevices+1)*sizeof(e1000_device), nullptr);
            //     OBOS_Warning("%02x:%02x:%02x: Bogus E1000 PCI node.", dev->location.bus, dev->location.slot, dev->location.function);
            //     dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
            //     continue;
            // }

            e1000_power_up_phy(&Devices[nDevices-1].hw);
            e1000_disable_ulp_lpt_lp(&Devices[nDevices-1].hw, true);

            Devices[nDevices-1].rx_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            Devices[nDevices-1].tx_done_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
            Devices[nDevices-1].irq_res = irq_res;
            Core_IrqObjectInitializeIRQL(&Devices[nDevices-1].irq, IRQL_E1000, true, true);
            Devices[nDevices-1].irq.handler = e1000_irq_handler;
            Devices[nDevices-1].irq.irqChecker = e1000_check_irq_callback;
            Devices[nDevices-1].irq.irqCheckerUserdata = Devices + nDevices - 1;
            Devices[nDevices-1].irq.handlerUserdata = Devices + nDevices - 1;
            Devices[nDevices-1].irq_res->irq->irq = &Devices[nDevices-1].irq;
            Devices[nDevices-1].irq_res->irq->masked = false;
            Drv_PCISetResource(Devices[nDevices-1].irq_res);
            Devices[nDevices-1].irq.handler = e1000_irq_handler;
            Devices[nDevices-1].irq.irqChecker = e1000_check_irq_callback;
            Devices[nDevices-1].irq.irqCheckerUserdata = Devices + nDevices - 1;
            Devices[nDevices-1].irq.handlerUserdata = Devices + nDevices - 1;

            e1000_init_tx(&Devices[nDevices-1]);
            e1000_init_rx(&Devices[nDevices-1]);

            e1000_clear_hw_cntrs_base_generic(&Devices[nDevices-1].hw);
            E1000_WRITE_REG(&Devices[nDevices-1].hw, E1000_IMS, IMS_ENABLE_MASK);
        }

        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    for (uint16_t bus = 0; bus < Drv_PCIBusCount; bus++)
        search_bus(&Drv_PCIBuses[bus]);
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