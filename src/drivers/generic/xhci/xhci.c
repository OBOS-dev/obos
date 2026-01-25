/*
 * drivers/generic/xhci/xhci.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/pci.h>
#include <driver_interface/header.h>

#include <allocators/base.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>
#include <irq/timer.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

#include <locks/wait.h>
#include <locks/event.h>
#include <locks/mutex.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <contrib/random.h>

#include <utils/list.h>

#define INIT_C
#include "xhci.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

static void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size_t phys_page_offset = (phys % OBOS_PAGE_SIZE);
    phys -= phys_page_offset;
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    size += phys_page_offset;
    obos_status status = OBOS_STATUS_SUCCESS;
    void* virt = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, size,
        uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
        nullptr, 
        &status);
    if (obos_is_error(status))
    {
        OBOS_Error("%s: Status %d\n", __func__, status);
        OBOS_ENSURE(virt);
    }
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

extern driver_id* this_driver;

static void process_port_attach(xhci_device* dev, uint8_t port_id);

void Sys_SleepMS(uint64_t ms, uint64_t*);

extern driver_header drv_hdr;

void xhci_probe_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev;)
    {
        if ((dev->hid.id & 0xffffffff) == (drv_hdr.pciId.id & 0xffffffff))
        {
            // intel chipsets
            if (dev->hid.indiv.vendorId == 0x8086)
            {
                OBOS_Debug("xhci: switching ehci ports to xhci\n");
                uint64_t usb3_ports_available = 0;
                uint64_t usb2_ports_available = 0;
                DrvS_ReadPCIRegister(dev->location, 0xdc, 4, &usb3_ports_available);
                DrvS_WritePCIRegister(dev->location, 0xd8, 4, usb3_ports_available);
                
                DrvS_ReadPCIRegister(dev->location, 0xd4, 4, &usb2_ports_available);
                DrvS_WritePCIRegister(dev->location, 0xd0, 4, usb2_ports_available);
            }

            // Device match, try initialization of a device.
            xhci_device* device = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(xhci_device), nullptr);
            OBOS_ENSURE(device);
            device->dev = dev;
            if (obos_is_error(xhci_initialize_device(device)))
                Free(OBOS_KernelAllocator, device, sizeof(*device));
            else
                xhci_append_device(device);
        }
            
        dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
    }
}

obos_status xhci_initialize_device(xhci_device* dev)
{
    OBOS_ENSURE(dev);
    for (pci_resource* curr_res = LIST_GET_HEAD(&pci_resource_list, &dev->dev->resources); curr_res; )
    {
        switch (curr_res->type) {
            case PCI_RESOURCE_BAR:
                if (curr_res->bar->idx == 0)
                    dev->pci_bar = curr_res;
                break;
            case PCI_RESOURCE_IRQ:
                dev->pci_irq = curr_res;
                break;
            default: break;
        }

        if (dev->pci_bar && dev->pci_irq)
            break;

        curr_res = LIST_GET_NEXT(pci_resource_list, &dev->dev->resources, curr_res);
    }

    if (!dev->pci_bar || !dev->pci_irq)
        return OBOS_STATUS_INTERNAL_ERROR;
    if (dev->pci_bar->bar->type == PCI_BARIO)
        return OBOS_STATUS_INTERNAL_ERROR; // this shouldn't happen

    OBOS_Log("xhci: Initializing XHCI controller at %02x:%02x:%02x\n", dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function);

    dev->trbs_inflight_lock = MUTEX_INITIALIZE();

    dev->base = map_registers(dev->pci_bar->bar->phys, dev->pci_bar->bar->size, false);
    dev->op_regs = (void*)((uintptr_t)dev->base + dev->cap_regs->caplength);
    dev->rt_regs = (void*)((uintptr_t)dev->base + dev->cap_regs->rtsoff);

    dev->dev->resource_cmd_register->cmd_register |= 0x6;
    Drv_PCISetResource(dev->dev->resource_cmd_register);

    // for (volatile bool b = true; b;)
    //     OBOSS_SpinlockHint();

    obos_status status = xhci_reset_device(dev);
    if (obos_is_error(status))
        return status;

    uint32_t hccparams1 = dev->cap_regs->hccparams1;
    dev->has_64bit_support = hccparams1 & BIT(0);
    dev->port_power_control_supported = hccparams1 & BIT(3);
    dev->xecp = hccparams1 >> 16;
    dev->hccparams1_csz = hccparams1 & BIT(2);
    dev->max_slots = dev->cap_regs->hcsparams1 & 0xff;

    if (dev->xecp)
    {
        uint32_t* current_cap = ((uint32_t*)dev->base) + dev->xecp;
        while (1)
        {
            if (!((*current_cap >> 8) & 0xff))
                break;
            if ((*current_cap & 0xff) != 0x2)
                goto next_cap;
            
            uint32_t* dw0 = &current_cap[0];
            uint32_t* dw2 = &current_cap[2];
            uint32_t poffset = (*dw2) & 0xff;
            uint32_t pcount = (*dw2 >> 8) & 0xff;

            if ((((*dw0) >> 24) & 0xff) == 0x3)
            {
                OBOS_ENSURE(!dev->usb3_port_start && "multiple USB3 regions unsupported");
                dev->usb3_port_start = poffset;
                dev->usb3_port_count = pcount;
            }
            else
            {
                OBOS_ENSURE(!dev->usb2_port_start && "multiple USB2 regions unsupported");
                dev->usb2_port_start = poffset;
                dev->usb2_port_count = pcount;
            }

            for (uint32_t curr = poffset; curr < (poffset+pcount); curr++)
            {
                xhci_port_info* pi = &dev->port_info[curr-1];
                if (pi->valid)
                {
                    OBOS_Warning("xhci: while initializing port info: found overlapping protocols in capability registers\n");
                    continue;
                }
                pi->usb3 = (((*dw0) >> 24) & 0xff) == 0x3;
                if (!pi->usb3)
                    pi->high_speed_only = (*dw2) & BIT(17);
                pi->valid = true;
                pi->active = true;
            }

            next_cap:
            current_cap += (*current_cap >> 8) & 0xff;
        };

        for (int i = 0; i < dev->usb3_port_count; i++)
        {
            xhci_port_info* pi = &dev->port_info[i+(dev->usb3_port_start-1)];
            if (!pi->valid)
                continue;
            if (i > dev->usb2_port_count)
            {
                pi->active = true;
                continue;
            }

            xhci_port_info* cpi = &dev->port_info[i+(dev->usb2_port_start-1)];
            
            pi->has_pair = true;
            pi->active = true;
            pi->companion_port = i+dev->usb2_port_start;
            cpi->has_pair = true;
            cpi->active = false;
            cpi->companion_port = i+dev->usb3_port_start;
        }
    }
        
    dev->slots = ZeroAllocate(OBOS_KernelAllocator, 256, sizeof(*dev->slots), nullptr);

    // Initialize the device context array
    OBOS_STATIC_ASSERT(OBOS_PAGE_SIZE >= 2048, "unimplemented");
    dev->device_context_array.pg = MmH_PgAllocatePhysical(dev->has_64bit_support, false);
    OBOS_ENSURE(dev->device_context_array.pg);
    dev->device_context_array.virt = MmS_MapVirtFromPhys(dev->device_context_array.pg->phys);
    dev->device_context_array.len = OBOS_PAGE_SIZE;
    memzero(dev->device_context_array.virt, dev->device_context_array.len);

    // Initialize the scratch pad
    const uint32_t nPages = (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) > OBOS_PAGE_SIZE ? (((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) / OBOS_PAGE_SIZE) + !!((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) % OBOS_PAGE_SIZE)) : 1;
    uint16_t scratch_pad_size = (dev->cap_regs->hcsparams2 >> 27) & 0x1f;
    scratch_pad_size |= ((dev->cap_regs->hcsparams2 >> 21) & 0x1f) << 5;
    if (scratch_pad_size)
    {
        dev->device_context_array.base[0].scratchpad_array_base = xhci_allocate_pages(xhci_page_count_for_size(scratch_pad_size, nPages), nPages, dev);

        uint64_t* scratch_pad_array = MmS_MapVirtFromPhys(dev->device_context_array.base[0].scratchpad_array_base);
        uint64_t buf = xhci_allocate_pages(xhci_page_count_for_size(scratch_pad_size * (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)), nPages), nPages, dev);
        memzero(MmS_MapVirtFromPhys(buf), (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) * scratch_pad_size);
        for (size_t i = 0; i < scratch_pad_size; i++)
            scratch_pad_array[i] = buf + i*(1 << (__builtin_ctz(dev->op_regs->pagesize)+12));
    }
    
    dev->op_regs->dcbaap = dev->device_context_array.pg->phys;

    dev->command_ring.pg = MmH_PgAllocatePhysical(!dev->has_64bit_support, false);
    OBOS_ENSURE(dev->command_ring.pg);
    dev->command_ring.virt = MmS_MapVirtFromPhys(dev->command_ring.pg->phys);
    dev->command_ring.len = OBOS_PAGE_SIZE;
    dev->command_ring.enqueue_ptr = dev->command_ring.pg->phys;
    dev->command_ring.dequeue_ptr = dev->command_ring.pg->phys;
    memzero(dev->command_ring.virt, dev->command_ring.len);
    dev->command_ring.rcs = true;

    do {
        uint32_t* trb = (dev->command_ring.virt);
        trb[3] |= BIT(0);
        trb += ((dev->command_ring.len-0x10) / 4);
        XHCI_SET_TRB_TYPE(trb, XHCI_TRB_LINK);
        trb[3] |= BIT(1);
        trb[0] = dev->command_ring.pg->phys & 0xffffffff;
        trb[1] = dev->command_ring.pg->phys >> 32;
    } while(0);

    dev->op_regs->crcr = dev->command_ring.dequeue_ptr | BIT(0);
    
    dev->event_ring.pg = MmH_PgAllocatePhysical(!dev->has_64bit_support, false);
    OBOS_ENSURE(dev->event_ring.pg);
    dev->event_ring.virt = MmS_MapVirtFromPhys(dev->event_ring.pg->phys);
    dev->event_ring.len = OBOS_PAGE_SIZE;
    dev->event_ring.nEntries = (OBOS_PAGE_SIZE-0x40) / 16;
    OBOS_ENSURE(dev->event_ring.nEntries >= 16 && dev->event_ring.nEntries < 4096);
    memzero(dev->event_ring.virt, dev->event_ring.len);

    Core_IrqObjectInitializeIRQL(&dev->irq, IRQL_XHCI, true, true);
    dev->irq.irqChecker = xhci_irq_checker;
    dev->irq.handler = xhci_irq_handler;
    dev->irq.handlerUserdata = dev;
    dev->irq.irqCheckerUserdata = dev;
    dev->pci_irq->irq->irq = &dev->irq;
    dev->pci_irq->irq->masked = false;
    Drv_PCISetResource(dev->pci_irq);
    dev->irq.irqChecker = xhci_irq_checker;
    dev->irq.handler = xhci_irq_handler;
    dev->irq.handlerUserdata = dev;
    dev->irq.irqCheckerUserdata = dev;

    dev->op_regs->dnctrl = BIT(1);
   
    // Set MaxSlotsEn to dev->max_slots
    dev->op_regs->config = (dev->max_slots) & 0xff;

    xhci_event_ring_segment_table_entry* ent = MmS_MapVirtFromPhys(dev->event_ring.pg->phys + (OBOS_PAGE_SIZE-0x40));
    ent->rsba = dev->event_ring.pg->phys;
    ent->rss = dev->event_ring.nEntries;

    dev->rt_regs->interrupters[0].erdp = dev->event_ring.pg->phys;
    dev->rt_regs->interrupters[0].erstsz = 1;
    
    dev->rt_regs->interrupters[0].imod = 4000;
    dev->rt_regs->interrupters[0].iman = BIT(1);

    dev->event_ring.rcs = true;

    status = Drv_USBControllerRegister(dev, &this_driver->header, &dev->ctlr);

    dev->op_regs->usbcmd |= (USBCMD_RUN|USBCMD_INTE|USBCMD_HSEE);
    if (!poll_bit_timeout(&dev->op_regs->usbsts, USBSTS_HCH, 0, 1000*1000))
    {
        OBOS_Error("xhci: timed out waiting for controller to start\n");
        xhci_reset_device(dev);
        return status;
    }
    dev->rt_regs->interrupters[0].erstba = dev->event_ring.pg->phys + (OBOS_PAGE_SIZE-0x40);

    for (uint8_t p = dev->usb3_port_start; p < (dev->usb3_port_start + dev->usb3_port_count); p++)
        xhci_reset_port(dev, p);
    for (uint8_t p = dev->usb2_port_start; p < (dev->usb2_port_start + dev->usb2_port_count); p++)
        xhci_reset_port(dev, p);

    size_t uninitialized_port_count = 0;

    for (uint8_t p = dev->usb3_port_start; p < (dev->usb3_port_start + dev->usb3_port_count); p++)
    {
        if (dev->port_info[p-1].active && dev->op_regs->ports[p-1].port_sc & PORTSC_CCS)
        {
            uninitialized_port_count++;
            process_port_attach(dev, p);
        }
    }

    for (uint8_t p = dev->usb2_port_start; p < (dev->usb2_port_start + dev->usb2_port_count); p++)
    {
        if (dev->port_info[p-1].active && dev->op_regs->ports[p-1].port_sc & PORTSC_CCS)
        {
            uninitialized_port_count++; 
            process_port_attach(dev, p);
        }
    }

    dev->allow_port_status_change_process = true;

    while (dev->initialized_ports_count < uninitialized_port_count)
        Sys_SleepMS(50, nullptr);

    return status;
}

obos_status xhci_reset_port(xhci_device* dev, uint8_t port_id)
{
    volatile xhci_port_registers* port = &dev->op_regs->ports[port_id-1];
    xhci_port_info* pi = &dev->port_info[port_id-1];

    if (!pi->active)
        return OBOS_STATUS_UNINITIALIZED;

    if (~port->port_sc & PORTSC_PP)
    {
        port->port_sc |= PORTSC_PP;
        Sys_SleepMS(20, nullptr);
        if (~port->port_sc & PORTSC_PP)
            return OBOS_STATUS_INTERNAL_ERROR;
    }

    port->port_sc = PORTSC_PP | (port->port_sc & PORTSC_CHANGE_BITS);

    uint32_t reset_bit = pi->usb3 ? PORTSC_WPR : PORTSC_PR;
    uint32_t reset_done_bit = PORTSC_PRC;

    port->port_sc = PORTSC_PP | reset_bit;

    if (!poll_bit_timeout(&port->port_sc, reset_done_bit, reset_done_bit, 500*1000))
    {
        pi->active = false;
        if (pi->usb3 && pi->has_pair)
            dev->port_info[pi->companion_port-1].active = true;
        OBOS_Debug("xhci: port reset timed out\n");
        return OBOS_STATUS_INTERNAL_ERROR;
    }

    Sys_SleepMS(20, nullptr);

    pi->active = (port->port_sc & PORTSC_PED);
    
    port->port_sc = PORTSC_PP | (port->port_sc & PORTSC_CHANGE_BITS);
    
    if (!pi->active && pi->usb3 && pi->has_pair)
        dev->port_info[pi->companion_port-1].active = true;

    return OBOS_STATUS_SUCCESS;
}

#define AUTO_RETRY(expr) \
({\
    obos_status _status = OBOS_STATUS_SUCCESS;\
    do {\
        _status = (expr);\
        Sys_SleepMS(10, nullptr);\
    } while (_status == OBOS_STATUS_WOULD_BLOCK); (_status);\
})

static void continue_port_attach_impl(uintptr_t *userdata)
{
    xhci_device* dev = (void*)userdata[0];
    uint8_t port_number = userdata[1];
    bool usb3 = userdata[2];
    Free(OBOS_KernelAllocator, userdata, sizeof(*userdata)*3);
    
    xhci_enable_slot_command_trb trb = {};
    xhci_inflight_trb* itrb = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;
    
    XHCI_SET_TRB_TYPE(&trb, XHCI_TRB_ENABLE_SLOT_COMMAND);

    status = AUTO_RETRY(xhci_trb_enqueue_command(dev, (void*)&trb, &itrb, true));
    if (obos_is_error(status))
        Core_ExitCurrentThread();
    
    status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
    if (obos_is_error(status))
    {
        if (itrb->resp)
            Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
        RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
        Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
        dev->initialized_ports_count++;
        Core_ExitCurrentThread();
    }
    
    xhci_commmand_completion_event_trb* resp = (void*)itrb->resp;
    
    uint8_t slot = (resp->dw3 >> 24) & 0xff;
    status = xhci_slot_initialize(dev, slot, port_number, (port_number<<24), nullptr);

    Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
    RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
    Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
    resp = nullptr;
    
    if (obos_is_success(status))
        OBOS_Debug("xhci: port attached\n");
    else
    {
        dev->initialized_ports_count++;
        OBOS_Debug("xhci: could not initialize slot %d: %d\n", slot, status);
        xhci_slot_free(dev, slot);
        Core_ExitCurrentThread();
    }

    OBOS_ENSURE(dev->ctlr);

    uint8_t pspeed = (dev->op_regs->ports[port_number-1].port_sc >> 10) & 0xf;
    uint8_t speed = 0;
    switch (pspeed) {
        case 1: speed = USB_DEVICE_FULL_SPEED; break;
        case 2: speed = USB_DEVICE_LOW_SPEED; break;
        case 3: speed = USB_DEVICE_HIGH_SPEED; break;
        case 4: speed = USB_DEVICE_SUPER_SPEED_GEN1_X1; break;
        case 5: speed = USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X1; break;
        case 6: speed = USB_DEVICE_SUPER_SPEED_PLUS_GEN1_X2; break;
        case 7: speed = USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X2; break;
        default: OBOS_ENSURE(!"unimplemented port speed value");
    }

    usb_device_info info = {};
    info.address = dev->slots[slot-1].address;
    info.slot = slot;
    info.port = slot;
    info.speed = speed;
    info.usb3 = usb3;

    if (obos_is_success(Drv_USBPortAttached(dev->ctlr, &info, &dev->slots[slot-1].desc, nullptr /* root hub */)))
        Drv_USBPortPostAttached(dev->ctlr, dev->slots[slot-1].desc);

    dev->initialized_ports_count++;

    Core_ExitCurrentThread();
}

static void continue_port_attach(xhci_device* dev, uint8_t port_id, bool usb3)
{
    uintptr_t* userdata = ZeroAllocate(OBOS_KernelAllocator, 3, sizeof(uintptr_t), nullptr);
    userdata[0] = (uintptr_t)dev;
    userdata[1] = port_id;
    userdata[2] = usb3;

    thread* thr = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, 0, 0x2000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
    CoreS_SetupThreadContext(&ctx, (uintptr_t)continue_port_attach_impl, (uintptr_t)userdata, false, stack, 0x2000);
    CoreH_ThreadInitialize(thr, THREAD_PRIORITY_REAL_TIME, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id), &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, thr);
    thr->stackFree = CoreH_VMAStackFree;
    thr->stackFreeUserdata = &Mm_KernelContext;
    CoreH_ThreadReady(thr);
}

static void process_port_attach(xhci_device* dev, uint8_t port_id)
{
    OBOS_UNUSED(dev && port_id);
    if (dev->allow_port_status_change_process)
        xhci_reset_port(dev, port_id);
    continue_port_attach(dev, port_id, dev->port_info[port_id-1].usb3);
}

static void process_port_detach_worker(uintptr_t *userdata)
{
    xhci_device* dev = (void*)userdata[0];
    uint8_t port_id = userdata[1];
    uint8_t slot = dev->port_info[port_id-1].slot;
    
    xhci_disable_slot_command_trb trb = {};
    xhci_inflight_trb* itrb = nullptr;
    
    XHCI_SET_TRB_TYPE(&trb, XHCI_TRB_DISABLE_SLOT_COMMAND);
    trb.dw3 |= (slot<<24);
    obos_status status = AUTO_RETRY(xhci_trb_enqueue_command(dev, (void*)&trb, &itrb, true));
    if (obos_is_error(status))
        Core_ExitCurrentThread();

    status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));

    Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
    RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
    Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));

    Drv_USBPortDetached(dev->ctlr, dev->slots[slot-1].desc);

    xhci_slot_free(dev, slot);

    if (!dev->port_info[port_id-1].usb3 && dev->port_info[port_id-1].has_pair)
    {
        dev->port_info[dev->port_info[port_id-1].companion_port-1].active = true;
        if (obos_is_success(xhci_reset_port(dev, dev->port_info[port_id-1].companion_port)))
            dev->port_info[port_id-1].active = false;
        else
            xhci_reset_port(dev, port_id);
    }
    else
        xhci_reset_port(dev, port_id);

    Core_ExitCurrentThread();
}

static void process_port_detach(xhci_device* dev, uint8_t port_id)
{
    uintptr_t* userdata = ZeroAllocate(OBOS_KernelAllocator, 2, sizeof(uintptr_t), nullptr);
    userdata[0] = (uintptr_t)dev;
    userdata[1] = port_id;

    thread* thr = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, 0, 0x2000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
    CoreS_SetupThreadContext(&ctx, (uintptr_t)process_port_detach_worker, (uintptr_t)userdata, false, stack, 0x2000);
    CoreH_ThreadInitialize(thr, THREAD_PRIORITY_REAL_TIME, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id), &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, thr);
    thr->stackFree = CoreH_VMAStackFree;
    thr->stackFreeUserdata = &Mm_KernelContext;
    CoreH_ThreadReady(thr);
}

static void process_port_status_change(xhci_device* dev, uint8_t port_id)
{
    if (!dev->allow_port_status_change_process)
        return;
    volatile xhci_port_registers* port = &dev->op_regs->ports[port_id-1];
    if (port->port_sc & PORTSC_CSC)
    {
        port->port_sc |= PORTSC_CSC;
        if (port->port_sc & PORTSC_CCS)
            process_port_attach(dev, port_id);
        else
            process_port_detach(dev, port_id);
    }
}

static void signal_inflight_trb(xhci_device* dev, uintptr_t dequeue_ptr, uintptr_t trb_ptr)
{
    if (!trb_ptr)
        return;
    xhci_inflight_trb key = {.ptr=trb_ptr,.ptr_end=trb_ptr+0x10};
    Core_MutexAcquire(&dev->trbs_inflight_lock);
    xhci_inflight_trb* itrb = RB_FIND(xhci_trbs_inflight, &dev->trbs_inflight, &key);
    if (!itrb)
    {
        OBOS_Debug("xhci: attempt to signal inflight TRB failed: TRB 0x%p not found\n", key.ptr);
        // OBOS_ASSERT(itrb);
        Core_MutexRelease(&dev->trbs_inflight_lock);
        return;
    } 
    RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
    memzero(&itrb->node, sizeof(itrb->node));
    if (itrb->dequeue_ptr && *itrb->dequeue_ptr < trb_ptr)
        *itrb->dequeue_ptr = trb_ptr;
    itrb->resp = memcpy(Allocate(OBOS_KernelAllocator, 16, nullptr), MmS_MapVirtFromPhys(dequeue_ptr), 16);
    itrb->resp_length = 4;
    Core_MutexRelease(&dev->trbs_inflight_lock);
    Core_EventSet(&itrb->evnt, false);
    if (XHCI_GET_COMPLETION_CODE(itrb->resp) != 1)
        OBOS_Debug("xhci: trb 0x%p completed with code 0x%x\n", trb_ptr, XHCI_GET_COMPLETION_CODE(itrb->resp));
}

static void process_command_completion_event(xhci_device* dev, uint32_t* trb)
{
    if (XHCI_GET_COMPLETION_CODE(trb) == 14 || XHCI_GET_COMPLETION_CODE(trb) == 15)
        return;
    signal_inflight_trb(dev, MmS_UnmapVirtFromPhys(trb), ((uint64_t)trb[0]) | (((uint64_t)trb[1]) << 32));
}

static __attribute__((alias("process_command_completion_event"))) void process_transfer_completion_event(xhci_device* dev, uint32_t* trb);

bool xhci_irq_checker(irq* i, void* udata)
{
    OBOS_UNUSED(i);
    xhci_device* dev = udata;
    return dev->op_regs->usbsts & 0x41C;
}

static void dpc_handler(dpc* d, void* userdata)
{
    OBOS_UNUSED(d);

    xhci_device* dev = userdata;

    volatile uint32_t usbsts = dev->op_regs->usbsts & 0x41C;
    uint32_t* curr_trb = MmS_MapVirtFromPhys(dev->rt_regs->interrupters[0].erdp & ~0xf);
    uint32_t* end = ((uint32_t*)dev->event_ring.virt) + (dev->event_ring.nEntries*4);
    // printf(__FILE__ ":%d %s erdp=0x%p usbsts=0x%x\n", __LINE__, __func__, curr_trb, usbsts);
    while (!!(curr_trb[3] & BIT(0)) == dev->event_ring.rcs && curr_trb < end)
    {
        switch (XHCI_GET_TRB_TYPE(curr_trb)) {
            case XHCI_TRB_PORT_STATUS_EVENT: process_port_status_change(dev, curr_trb[0] >> 24); break;
            case XHCI_TRB_COMMAND_COMPLETION_EVENT: process_command_completion_event(dev, curr_trb); break;
            case XHCI_TRB_TRANSFER_EVENT: process_transfer_completion_event(dev, curr_trb); break;
            default:
                OBOS_Debug("xhci: skipping unrecognized TRB type %d\n", XHCI_GET_TRB_TYPE(curr_trb));
                break;
        }
        curr_trb += 4;
    }
    if (curr_trb == end)
    {
        curr_trb = dev->event_ring.virt;
        dev->event_ring.rcs = !dev->event_ring.rcs;
    }
    
    dev->rt_regs->interrupters[0].erdp = MmS_UnmapVirtFromPhys(curr_trb) | BIT(3);

    dev->op_regs->usbsts |= usbsts;
    dev->handling_irq = false;
}

void xhci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && oldIrql);
    xhci_device* dev = userdata;
    dev->irq_dpc.userdata = dev;
    dev->handling_irq = true;
    CoreH_InitializeDPC(&dev->irq_dpc, dpc_handler, Core_DefaultThreadAffinity);
}

static obos_status do_bios_handoff(xhci_device* dev)
{
    if (dev->did_bios_handoff)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    if (!dev->xecp)
    {
        dev->did_bios_handoff = true;
        return OBOS_STATUS_SUCCESS;
    }

    uint32_t* current_cap = ((uint32_t*)dev->base) + dev->xecp;
    while (1)
    {
        if ((*current_cap & 0xff) == 0x1)
            break;
        if (!((*current_cap >> 8) & 0xff))
            goto done;
        current_cap += (*current_cap >> 8) & 0xff;
    };
    *current_cap |= BIT(24);

    if (!poll_bit_timeout(current_cap, BIT(16)|BIT(24), BIT(24), 1*1000*1000 /* 1 second */))
    {
        OBOS_Warning("xhci: %02x:%02x:%02x: BIOS handoff timed out after 1 second.\n",
            dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function
        );
        return OBOS_STATUS_SUCCESS;
    }
    
    done:
    // DrvS_WritePCIRegister(dev->dev->location, dev->xecp+4, 4, 0xE0000000 /* some bits are RW1C */);
    dev->did_bios_handoff = true;
    return OBOS_STATUS_SUCCESS;
}

void populate_trbs(irp* req, bool data_stage, xhci_normal_trb* trbs, size_t nRegions, struct physical_region *regions, xhci_endpoint_context* ep_ctx, bool in_endpoint);
obos_status xhci_slot_initialize(xhci_device* dev, uint8_t slot, uint8_t port, uint32_t route_string, const usb_hub_info* hub_info)
{
    if (dev->slots[slot-1].allocated)
    {
        OBOS_Warning("xhci: xhci_slot_initialize called on an allocated slot.\n");
        xhci_slot_free(dev, slot);
    }

    dev->slots[slot-1].trb_ring[0].buffer.pg = MmH_PgAllocatePhysical(!dev->has_64bit_support, false);
    OBOS_ENSURE(dev->slots[slot-1].trb_ring[0].buffer.pg);
    dev->slots[slot-1].trb_ring[0].buffer.virt = MmS_MapVirtFromPhys(dev->slots[slot-1].trb_ring[0].buffer.pg->phys);
    dev->slots[slot-1].trb_ring[0].buffer.len = OBOS_PAGE_SIZE;
    memzero(dev->slots[slot-1].trb_ring[0].buffer.virt, dev->slots[slot-1].trb_ring[0].buffer.len);
    dev->slots[slot-1].trb_ring[0].enqueue_ptr = dev->slots[slot-1].trb_ring[0].buffer.pg->phys;
    dev->slots[slot-1].trb_ring[0].dequeue_ptr = dev->slots[slot-1].trb_ring[0].buffer.pg->phys;
    dev->slots[slot-1].trb_ring[0].ccs = true;
    
    uint32_t* link_trb = (dev->slots[slot-1].trb_ring[0].buffer.virt);
    link_trb[3] |= BIT(0);
    link_trb += ((dev->slots[slot-1].trb_ring[0].buffer.len-0x10) / 4);
    XHCI_SET_TRB_TYPE(link_trb, XHCI_TRB_LINK);
    link_trb[3] |= BIT(1);
    link_trb[0] = dev->slots[slot-1].trb_ring[0].buffer.pg->phys & 0xffffffff;
    link_trb[1] = dev->slots[slot-1].trb_ring[0].buffer.pg->phys >> 32;

    dev->slots[slot-1].doorbell = ((uint32_t*)dev->base) + dev->cap_regs->dboff/4 + slot;

    const uint32_t nPages = (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) > OBOS_PAGE_SIZE ? (((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) / OBOS_PAGE_SIZE) + !!((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) % OBOS_PAGE_SIZE)) : 1;

    // Initialize the device context
    const size_t sz = dev->hccparams1_csz ? 0x800 : 0x400;
    dev->device_context_array.base[slot].device_context_base = xhci_allocate_pages(xhci_page_count_for_size(sz, nPages), nPages, dev);
    void* device_context = MmS_MapVirtFromPhys(dev->device_context_array.base[slot].device_context_base);
    memzero(device_context, xhci_page_count_for_size(sz, nPages)*OBOS_PAGE_SIZE);

    const size_t sz2 = dev->hccparams1_csz ? 0x840 : 0x420;
    uintptr_t input_context_base = xhci_allocate_pages(xhci_page_count_for_size(sz2, nPages), nPages, dev);
    xhci_input_context* input_context = MmS_MapVirtFromPhys(input_context_base);
    memzero(input_context, xhci_page_count_for_size(sz2, nPages)*OBOS_PAGE_SIZE);
    input_context->icc.add_context |= 3;

    uint8_t rh_port = route_string >> 24;
    dev->port_info[rh_port-1].active = true;

    dev->port_info[rh_port-1].slot = slot;

    xhci_slot_context* slot_ctx = get_xhci_endpoint_context(dev, input_context, 1);
    // TODO: correct value
    slot_ctx->dw0 = (route_string & 0xfffff) << 0;
    // Context entries = 1
    slot_ctx->dw0 |= 1<<27;
    slot_ctx->dw1 |= (rh_port<<16);
    if (hub_info)
    {
        slot_ctx->dw0 |= BIT(26);
        if (hub_info->mtt)
        {
            slot_ctx->dw0 |= BIT(25);
            slot_ctx->dw2 |= (hub_info->tt_think_time&3) << 16;
        }
    }
    slot_ctx->dw2 |= ((uint32_t)port) << 8;
    slot_ctx->dw2 |= ((uint32_t)dev->port_info[rh_port-1].slot) << 0;

    xhci_endpoint_context* ctrl_ep = get_xhci_endpoint_context(dev, input_context, 2);
    // CErr=3
    ctrl_ep->flags2 |= (0x3<<1);
    // EPType = Control (4)
    ctrl_ep->flags2 |= (0x4<<3);
    ctrl_ep->average_trb_length = 8;
    if ((route_string & 0xfffff) == 0)
    {
        uint8_t pspeed = (dev->op_regs->ports[port-1].port_sc >> 10) & 0xf;
        switch (pspeed) {
            case 1:
                ctrl_ep->max_packet_size = 8;
                break;
            case 2:
                ctrl_ep->max_packet_size = 8;
                break;
            case 3:
                ctrl_ep->max_packet_size = 64;
                break;
            case 4:
            case 5:
            case 6:
            case 7:
                ctrl_ep->max_packet_size = 512;
                break;
            default: OBOS_ENSURE(!"unimplemented or invalid port speed");
        }
        slot_ctx->dw0 |= ((uint32_t)pspeed) << 20;
    }
    else
        ctrl_ep->max_packet_size = 8;
    // DCS=1
    ctrl_ep->tr_dequeue_pointer = dev->slots[slot-1].trb_ring[0].enqueue_ptr | BIT(0);

    xhci_address_device_command_trb trb = {};
    XHCI_SET_TRB_TYPE(&trb, XHCI_TRB_ADDRESS_DEVICE_COMMAND);
    trb.icp = input_context_base;
    trb.dw3 |= (slot<<24);
    trb.dw3 |= BIT(9);
    
    xhci_inflight_trb* itrb = nullptr;
    obos_status status = AUTO_RETRY(xhci_trb_enqueue_command(dev, (void*)&trb, &itrb, true));
    if (obos_is_error(status))
        return status;
    status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
    if (obos_is_error(status))
    {
        if (itrb->resp)
            Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
        RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
        RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
        Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
        return status;
    }

    if (XHCI_GET_COMPLETION_CODE(itrb->resp) != 1)
    {
        OBOS_Debug("xhci: could not address device on slot %d. completion code=%d\n", slot, XHCI_GET_COMPLETION_CODE(itrb->resp));
        status = OBOS_STATUS_INTERNAL_ERROR;
    }

    trb.dw3 &= ~BIT(9);

    if (obos_is_success(status))
    {
        Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
        RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
        Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));

        OBOS_ALIGNAS(32) uint8_t buf[8] = {};
        usb_irp_payload payload = {};
        payload.trb_type = USB_TRB_CONTROL;
        payload.payload.setup.bmRequestType = 0x80;
        payload.payload.setup.bRequest = USB_GET_DESCRIPTOR;
        payload.payload.setup.wValue = ((uint16_t)USB_DESCRIPTOR_TYPE_DEVICE << 8);
        payload.payload.setup.wLength = 8;
        DrvH_ScatterGather(&Mm_KernelContext, buf, 8, &payload.payload.setup.regions, &payload.payload.setup.nRegions, 1, false);

        do {
            xhci_setup_stage_trb trb1 = {};
            XHCI_SET_TRB_TYPE(&trb1, XHCI_TRB_SETUP_STAGE);
            trb1.bmRequestType = payload.payload.setup.bmRequestType;
            trb1.bRequest = payload.payload.setup.bRequest;
            trb1.wValue = payload.payload.setup.wValue;
            trb1.wLength = payload.payload.setup.wLength;
            trb1.length = 8;
            trb1.flags_type |= BIT(6);
            trb1.trt = 3;

            xhci_data_stage_trb trb2 = {};
            XHCI_SET_TRB_TYPE(&trb2, XHCI_TRB_DATA_STAGE);
            trb2.dbp = payload.payload.setup.regions[0].phys;
            trb2.length_td_size = payload.payload.setup.regions[0].sz;
            trb2.dir_resv = 1;
            
            xhci_status_stage_trb trb3 = {};
            XHCI_SET_TRB_TYPE(&trb3, XHCI_TRB_STATUS_STAGE);
            trb3.dir_resv = 1;
            trb3.flags_type |= BIT(5);

            xhci_trb_enqueue_slot(dev,
                                  slot-1, 0,
                                  XHCI_DIRECTION_IN,
                                  (uint32_t*)&trb1,
                                  &itrb,
                                false);
            xhci_trb_enqueue_slot(dev,
                                  slot-1, 0,
                                  XHCI_DIRECTION_IN,
                                  (uint32_t*)&trb2,
                                  &itrb,
                                false);
            xhci_trb_enqueue_slot(dev,
                                  slot-1, 0,
                                  XHCI_DIRECTION_IN,
                                  (uint32_t*)&trb3,
                                  &itrb,
                                false);
            itrb->ptr -= 0x20;
            xhci_doorbell_slot(&dev->slots[slot-1], 0, 0);
            
            status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
            
            uint8_t completion_code = XHCI_GET_COMPLETION_CODE(itrb->resp);

            Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
            RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
            Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));

            if (completion_code != 1)
                status = OBOS_STATUS_INTERNAL_ERROR;
        } while(0);
        ctrl_ep->tr_dequeue_pointer = dev->slots[slot-1].trb_ring[0].enqueue_ptr | BIT(0);

        if (obos_is_error(status))
            return status; // TODO: is anything leaked here?

        if (buf[7] != ctrl_ep->max_packet_size)
        {
            ctrl_ep->max_packet_size = buf[7];
            xhci_evaluate_context_command_trb ecc_trb = {};
            XHCI_SET_TRB_TYPE(&ecc_trb, XHCI_TRB_EVALUATE_CONTEXT_COMMAND);
            ecc_trb.dw3 |= (slot<<24);                
            ecc_trb.icp = input_context_base;
            xhci_trb_enqueue_command(dev, (void*)&ecc_trb, &itrb, true);
            
            status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
            Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
            RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
            Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
        }
        DrvH_FreeScatterGatherList(&Mm_KernelContext, buf, 8, payload.payload.setup.regions, payload.payload.setup.nRegions);

        if ((route_string & 0xfffff) == 0)
        {
            status = xhci_reset_port(dev, rh_port);
            if (obos_is_error(status))
                return status;
        }

        status = AUTO_RETRY(xhci_trb_enqueue_command(dev, (void*)&trb, &itrb, true));
        if (obos_is_error(status))
            return status;
        status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
        if (obos_is_error(status))
        {
            if (itrb->resp)
                Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
            RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
            Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
            return status;
        }

        if (XHCI_GET_COMPLETION_CODE(itrb->resp) != 1)
        {
            OBOS_Debug("xhci: could not address device on slot %d. completion code=%d\n", slot, XHCI_GET_COMPLETION_CODE(itrb->resp));
            status = OBOS_STATUS_INTERNAL_ERROR;
        }

        if (obos_is_error(status))
        {
            Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
            RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
            Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
            goto down;
        }

        dev->slots[slot-1].address = slot_ctx->dw3 & 0xff;

        OBOS_Debug("%s: sucessfully initialized slot %d on port %d with address %d\n", __func__, slot, port, dev->slots[slot-1].address);
    }
    else
    {
        Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
        RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
        Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
    }

    down:
    Mm_FreePhysicalPages(input_context_base, xhci_page_count_for_size(sz2, nPages));

    return status;
}

obos_status xhci_slot_free(xhci_device* dev, uint8_t slot)
{
    if (!dev->slots[slot-1].allocated)
        return OBOS_STATUS_SUCCESS;

    for (int i = 0; i < 31; i++)
        if (dev->slots[slot-1].trb_ring[i].buffer.len)
            MmH_DerefPage(dev->slots[slot-1].trb_ring[i].buffer.pg);

    const uint32_t nPages = (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) > OBOS_PAGE_SIZE ? (((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) / OBOS_PAGE_SIZE) + !!((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) % OBOS_PAGE_SIZE)) : 1;
    const size_t sz = dev->hccparams1_csz ? 0x800 : 0x400;
    
    // TODO: Do we need to do anything else?

    Mm_FreePhysicalPages(dev->device_context_array.base[slot].device_context_base, xhci_page_count_for_size(sz, nPages));
    dev->device_context_array.base[slot].device_context_base = 0;    

    dev->slots[slot-1].allocated = false;

    OBOS_ENSURE(dev->slots[slot-1].port_id > 0);
    dev->port_info[dev->slots[slot-1].port_id-1].slot = 0;

    memzero(&dev->slots[slot-1], sizeof(dev->slots[slot-1]));

    return OBOS_STATUS_SUCCESS;
}

void xhci_doorbell_slot(xhci_slot* slot, uint8_t endpoint, xhci_direction direction)
{
    uint8_t db_target = endpoint == 0 ? 1 : (endpoint*2 + direction);
    if (db_target > 31)
        return;
    *slot->doorbell = (uint32_t)db_target;
}

void xhci_doorbell_control(xhci_device* dev)
{
    *((uint32_t*)dev->base + (dev->cap_regs->dboff/4)) = 0;
}

void* xhci_get_device_context(xhci_device* dev, uint8_t slot)
{
    return MmS_MapVirtFromPhys(dev->device_context_array.base[slot].device_context_base);
}

RB_GENERATE(xhci_trbs_inflight, xhci_inflight_trb, node, cmp_inflight_trb);

static xhci_inflight_trb* add_inflight_trb(xhci_device* dev, uintptr_t ptr)
{
    Core_MutexRelease(&dev->trbs_inflight_lock);
    xhci_inflight_trb* inflight = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*inflight), nullptr);
    Core_MutexAcquire(&dev->trbs_inflight_lock);
    inflight->ptr = ptr;
    inflight->ptr_end = ptr+0x10;
    inflight->resp = nullptr;
    inflight->resp_length = 0;
    memcpy(inflight->trb_cpy, MmS_MapVirtFromPhys(ptr), 16);
    inflight->evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    RB_INSERT(xhci_trbs_inflight, &dev->trbs_inflight, inflight);
    return inflight;
}

obos_status xhci_trb_enqueue_slot(xhci_device* dev, uint8_t slot_id, uint8_t endpoint, xhci_direction direction, uint32_t* trb, xhci_inflight_trb** itrb, bool doorbell)
{
    xhci_slot* slot = &dev->slots[slot_id];
    if (!trb || !dev || !itrb)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    // if (!slot->allocated)
    //     return OBOS_STATUS_UNINITIALIZED;

    const uint8_t target = endpoint == 0 ? 0 : ((endpoint+1)*2 + !direction);

    if (!slot->trb_ring[target].enqueue_ptr)
        return OBOS_STATUS_UNINITIALIZED;
    
    const uint8_t dci = (endpoint == 0 ? 1 : (endpoint*2 + direction));
    xhci_endpoint_context* ep_ctx = get_xhci_endpoint_context(dev, xhci_get_device_context(dev, slot_id+1), dci);
    uint64_t dequeue_ptr = ep_ctx->tr_dequeue_pointer & ~0xf;
    if ((slot->trb_ring[target].enqueue_ptr + (16)) == dequeue_ptr)
        return OBOS_STATUS_WOULD_BLOCK;

    Core_MutexAcquire(&dev->trbs_inflight_lock);

    void* ptr = MmS_MapVirtFromPhys(slot->trb_ring[target].enqueue_ptr);
    memcpy(ptr, trb, 16);
    if (trb[3] & BIT(5))
    {
        *itrb = add_inflight_trb(dev, slot->trb_ring[target].enqueue_ptr);
        (*itrb)->dequeue_ptr = &slot->trb_ring[target].dequeue_ptr;
    }
    else 
        *itrb = nullptr;
    if (slot->trb_ring[target].ccs)
        ((uint32_t*)ptr)[3] |= BIT(0);
    slot->trb_ring[target].enqueue_ptr += 16;
    if (slot->trb_ring[target].enqueue_ptr >= (slot->trb_ring[target].buffer.pg->phys + slot->trb_ring[target].buffer.len-0x10))
    {
        if (slot->trb_ring[target].ccs)
            ((uint32_t*)ptr)[3+4] |= BIT(0);
        else
            ((uint32_t*)ptr)[3+4] &= ~BIT(0);
        doorbell = true;
        slot->trb_ring[target].enqueue_ptr = slot->trb_ring[target].buffer.pg->phys;
        slot->trb_ring[target].ccs = !slot->trb_ring[target].ccs;
    }
    
    Core_MutexRelease(&dev->trbs_inflight_lock);

    if (doorbell)
        xhci_doorbell_slot(slot, endpoint, direction);

    return OBOS_STATUS_SUCCESS;
}

obos_status xhci_trb_enqueue_command(xhci_device* dev, uint32_t* trb, xhci_inflight_trb** itrb, bool doorbell)
{
    if (!dev || !trb || !itrb)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!dev->command_ring.pg)
        return OBOS_STATUS_UNINITIALIZED;
    
    if ((dev->command_ring.enqueue_ptr + (16)) == dev->command_ring.dequeue_ptr)
        return OBOS_STATUS_WOULD_BLOCK;

    Core_MutexAcquire(&dev->trbs_inflight_lock);

    void* ptr = MmS_MapVirtFromPhys(dev->command_ring.enqueue_ptr);
    memcpy(ptr, trb, 16);
    if (dev->command_ring.rcs)
        ((uint32_t*)ptr)[3] |= BIT(0); // cycle bit
    else
        ((uint32_t*)ptr)[3] &= ~BIT(0); // cycle bit
    *itrb = add_inflight_trb(dev, dev->command_ring.enqueue_ptr);
    (*itrb)->dequeue_ptr = &dev->command_ring.dequeue_ptr;
    dev->command_ring.enqueue_ptr += 16;
    if (dev->command_ring.enqueue_ptr >= (dev->command_ring.pg->phys + dev->command_ring.len-0x10))
    {
        if (dev->command_ring.rcs)
            ((uint32_t*)ptr)[3+4] |= BIT(0);
        else
            ((uint32_t*)ptr)[3+4] &= ~BIT(0);
        doorbell = true;
        dev->command_ring.enqueue_ptr = dev->command_ring.pg->phys;
        dev->command_ring.rcs = !dev->command_ring.rcs;
    }

    Core_MutexRelease(&dev->trbs_inflight_lock);
    
    if (doorbell)
        xhci_doorbell_control(dev);

    return OBOS_STATUS_SUCCESS;
}

obos_status xhci_reset_device(xhci_device* dev)
{
    OBOS_ENSURE(dev);
    if (!dev)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    obos_status status = OBOS_STATUS_SUCCESS;
    if (!dev->did_bios_handoff)
        if (obos_is_error(status = do_bios_handoff(dev)))
            return status;
    dev->op_regs->usbcmd &= ~USBCMD_RUN;
    while (~dev->op_regs->usbsts & USBSTS_HCH)
        OBOSS_SpinlockHint();
    dev->op_regs->usbcmd |= USBCMD_RESET;
    if (!poll_bit_timeout(&dev->op_regs->usbcmd, USBCMD_RESET, 0, 1000000))
    {
        OBOS_Error("xhci: could reset controller: timed out\n");
        return OBOS_STATUS_TIMED_OUT;
    }
    if (!poll_bit_timeout(&dev->op_regs->usbsts, USBSTS_CNR, 0, 1000000))
    {
        OBOS_Error("xhci: could reset controller: timed out\n");
        return OBOS_STATUS_TIMED_OUT;
    }
    OBOS_Log("xhci: Reset XHCI controller at %02x:%02x:%02x\n", dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function);
    return OBOS_STATUS_SUCCESS;
}

bool poll_bit_timeout(volatile uint32_t *field, uint32_t mask, uint32_t expected, uint32_t us_timeout)
{
    timer_tick deadline = us_timeout == UINT32_MAX ? UINT64_MAX : (CoreS_GetTimerTick() + CoreH_TimeFrameToTick(us_timeout));
    while ((*field & mask) != expected && CoreS_GetTimerTick() < deadline)
        Sys_SleepMS(1, nullptr);
    return (*field & mask) == expected;
}

obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    xhci_device* dev = (void*)what;
    switch (request) {
        case IOCTL_USB_CTLR_ALLOCATE_SLOT:
        {
            usb_ctlr_ioctl_slot_allocate *arg = argp;
            
            xhci_enable_slot_command_trb trb = {};
            xhci_inflight_trb* itrb = nullptr;
            obos_status status = OBOS_STATUS_SUCCESS;
            
            XHCI_SET_TRB_TYPE(&trb, XHCI_TRB_ENABLE_SLOT_COMMAND);

            status = AUTO_RETRY(xhci_trb_enqueue_command(dev, (void*)&trb, &itrb, true));
            if (obos_is_error(status))
                return status;

            status = Core_WaitOnObject(WAITABLE_OBJECT(itrb->evnt));
            if (obos_is_error(status))
            {
                if (itrb->resp)
                    Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
                RB_REMOVE(xhci_trbs_inflight, &dev->trbs_inflight, itrb);
                Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
                return status;
            }

            xhci_commmand_completion_event_trb* resp = (void*)itrb->resp;

            uint8_t slot = (resp->dw3 >> 24) & 0xff;
            status = xhci_slot_initialize(dev, slot, arg->port_number, arg->route_string, arg->is_hub ? &arg->hub_info : nullptr);
            if (obos_is_error(status))
                OBOS_Debug("xhci: could not initialize slot %d: %d\n", slot, status);
            else
                OBOS_Debug("xhci: port attached\n");

            arg->slot = slot;
            arg->address = dev->slots[slot-1].address;
    
            dev->initialized_ports_count++;

            break;
        }
        default: return OBOS_STATUS_INVALID_IOCTL;
    }
    return OBOS_STATUS_SUCCESS;
}