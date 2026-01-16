/*
 * drivers/x86_64/xhci/init.c
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

#include <contrib/random.h>

#include <utils/list.h>

#define INIT_C
#include "structs.h"

extern driver_header drv_hdr;

void xhci_probe_bus(pci_bus* bus)
{
    for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev;)
    {
        if ((dev->hid.id & 0xffffffff) == (drv_hdr.pciId.id & 0xffffffff))
        {
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

#define xhci_allocate_pages(nPages, alignment, dev) (dev->has_64bit_support ? Mm_AllocatePhysicalPages(nPages, alignment, nullptr) : Mm_AllocatePhysicalPages32(nPages, alignment, nullptr))
#define xhci_page_count_for_size(size, nPages) ((size) / (nPages) + !!((size) / (nPages)))

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

    OBOS_Log("XHCI: Initializing XHCI controller at %02x:%02x:%02x\n", dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function);

    dev->base = map_registers(dev->pci_bar->bar->phys, dev->pci_bar->bar->size, false);
    dev->op_regs = (void*)((uintptr_t)dev->base + dev->cap_regs->caplength);
    dev->rt_regs = (void*)((uintptr_t)dev->base + dev->cap_regs->rtsoff);

    dev->dev->resource_cmd_register->cmd_register |= 0x6;
    Drv_PCISetResource(dev->dev->resource_cmd_register);

    uint32_t hccparams1 = dev->cap_regs->hccparams1;
    dev->has_64bit_support = hccparams1 & BIT(0);
    dev->port_power_control_supported = hccparams1 & BIT(3);
    dev->xecp = (hccparams1 >> 16)*4;
    dev->hccparams1_csz = hccparams1 & BIT(2);
    dev->max_slots = dev->cap_regs->hcsparams1 & 0xff;

    obos_status status = xhci_reset_device(dev);
    if (obos_is_error(status))
        return status;

    dev->slots = ZeroAllocate(OBOS_KernelAllocator, 256, sizeof(*dev->slots), nullptr);

    // Set MaxSlotsEn to dev->max_slots
    dev->op_regs->config |= (dev->max_slots) & 0xff;

    // Initialize the device context array
    OBOS_STATIC_ASSERT(OBOS_PAGE_SIZE >= 2048, "unimplemented");
    dev->device_context_array.pg = MmH_PgAllocatePhysical(dev->has_64bit_support, false);
    OBOS_ENSURE(dev->device_context_array.pg);
    dev->device_context_array.virt = MmS_MapVirtFromPhys(dev->device_context_array.pg->phys);
    dev->device_context_array.len = OBOS_PAGE_SIZE;

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
    memzero(dev->command_ring.virt, dev->command_ring.len);

    dev->op_regs->crcr = dev->command_ring.pg->phys | BIT(0);
    
    dev->event_ring.pg = MmH_PgAllocatePhysical(!dev->has_64bit_support, false);
    OBOS_ENSURE(dev->event_ring.pg);
    dev->event_ring.virt = MmS_MapVirtFromPhys(dev->event_ring.pg->phys);
    dev->event_ring.len = OBOS_PAGE_SIZE;
    dev->event_ring.nEntries = (OBOS_PAGE_SIZE-0x40) / sizeof(xhci_nop_trb);
    OBOS_ENSURE(dev->event_ring.nEntries >= 16 && dev->event_ring.nEntries < 4096);
    memzero(dev->event_ring.virt, dev->event_ring.len);

    xhci_event_ring_segment_table_entry* ent = MmS_MapVirtFromPhys(dev->event_ring.pg->phys + (OBOS_PAGE_SIZE-0x40));
    ent->rsba = dev->event_ring.pg->phys;
    ent->rss = dev->event_ring.nEntries;

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

    dev->rt_regs->interrupters[0].erstba = dev->event_ring.pg->phys + (OBOS_PAGE_SIZE-0x40);
    dev->rt_regs->interrupters[0].erstsz = 1;
    dev->rt_regs->interrupters[0].erdp = dev->event_ring.pg->phys;

    dev->rt_regs->interrupters[0].iman |= BIT(1);
    dev->rt_regs->interrupters[0].imod = 4000;

    dev->op_regs->usbcmd |= (USBCMD_RUN|USBCMD_INTE);

    return status;
}

bool xhci_irq_checker(irq* i, void* udata)
{
    OBOS_UNUSED(i);
    xhci_device* dev = udata;
    return dev->op_regs->usbsts & 0x41C;
}
void xhci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && oldIrql);
    xhci_device* dev = userdata;
    printf("XHCI Interrupt\n");
    dev->op_regs->usbsts |= dev->op_regs->usbsts;
    return;
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

    uint32_t* current_cap = (uint32_t*)dev->base + dev->xecp/4;
    while (1)
    {
        if ((*current_cap & 0xff) == 0x1)
            break;
        if (!((*current_cap >> 8) & 0xff))
            goto done;
        current_cap += (*current_cap >> 8) & 0xff;
    };
    *current_cap |= BIT(24);

    if (!poll_bit_timeout(current_cap, BIT(16), 0, 1*1000*1000 /* 1 second */))
    {
        OBOS_Error("XHCI: %02x:%02x:%02x: BIOS handoff timed out after 1 second.",
            dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function
        );
        return OBOS_STATUS_TIMED_OUT;
    }

    *(current_cap+1) = 0xE0000000;
    
    done:
    // DrvS_WritePCIRegister(dev->dev->location, dev->xecp+4, 4, 0xE0000000 /* some bits are RW1C */);
    dev->did_bios_handoff = true;
    return OBOS_STATUS_SUCCESS;
}

obos_status xhci_slot_initialize(xhci_device* dev, uint8_t slot)
{
    if (dev->slots[slot].allocated)
        xhci_slot_free(dev, slot);

    dev->slots[slot].trb_ring[0].buffer.pg = MmH_PgAllocatePhysical(!dev->has_64bit_support, false);
    OBOS_ENSURE(dev->slots[slot].trb_ring[0].buffer.pg);
    dev->slots[slot].trb_ring[0].buffer.virt = MmS_MapVirtFromPhys(dev->slots[slot].trb_ring[0].buffer.pg->phys);
    dev->slots[slot].trb_ring[0].buffer.len = OBOS_PAGE_SIZE;
    memzero(dev->slots[slot].trb_ring[0].buffer.virt, dev->slots[slot].trb_ring[0].buffer.len);
    dev->slots[slot].trb_ring[0].enqueue_ptr = dev->slots[slot].trb_ring[0].buffer.pg->phys;
    dev->slots[slot].doorbell = (uint32_t*)(((uint8_t*)dev->base) + dev->cap_regs->dboff) + slot;

    const uint32_t nPages = (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) > OBOS_PAGE_SIZE ? (((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) / OBOS_PAGE_SIZE) + !!((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) % OBOS_PAGE_SIZE)) : 1;

    // Initialize the device context
    size_t sz = dev->hccparams1_csz ? 0x800 : 0x400;
    dev->device_context_array.base[slot].device_context_base = xhci_allocate_pages(xhci_page_count_for_size(sz, nPages), nPages, dev);
    void* device_context = MmS_MapVirtFromPhys(dev->device_context_array.base[slot+1].device_context_base);

    xhci_endpoint_context* ctrl_ep = get_xhci_endpoint_context(dev, device_context, 1);
    // CErr=3
    ctrl_ep->flags2 |= (0x3<<1);
    // EPType = Control (4)
    ctrl_ep->flags2 |= (0x4<<3);
    // DCS=1
    ctrl_ep->tr_dequeue_pointer |= BIT(0);
    ctrl_ep->tr_dequeue_pointer |= dev->slots[slot].trb_ring[0].enqueue_ptr;

    return OBOS_STATUS_SUCCESS;
}

obos_status xhci_slot_free(xhci_device* dev, uint8_t slot)
{
    if (!dev->slots[slot].allocated)
        return OBOS_STATUS_SUCCESS;

    for (int i = 0; i < 16; i++)
        if (dev->slots[slot].trb_ring[i].buffer.len)
            MmH_DerefPage(dev->slots[slot].trb_ring[i].buffer.pg);

    const uint32_t nPages = (1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) > OBOS_PAGE_SIZE ? (((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) / OBOS_PAGE_SIZE) + !!((1 << (__builtin_ctz(dev->op_regs->pagesize)+12)) % OBOS_PAGE_SIZE)) : 1;
    const size_t sz = dev->hccparams1_csz ? 0x800 : 0x400;
    
    // TODO: Do we need to do anything else?

    Mm_FreePhysicalPages(dev->device_context_array.base[slot+1].device_context_base, xhci_page_count_for_size(sz, nPages));
    dev->device_context_array.base[slot+1].device_context_base = 0;    

    dev->slots[slot].allocated = false;
    memzero(&dev->slots[slot], sizeof(dev->slots[slot]));

    return OBOS_STATUS_SUCCESS;
}
void xhci_doorbell_slot(xhci_slot* slot, uint8_t endpoint, bool direction /* true for OUT, false for IN */)
{
    uint8_t db_target = endpoint == 0 ? 1 : ((endpoint+1)*2 + !direction);
    if (db_target > 31)
        return;
    *slot->doorbell = (uint32_t)db_target;
}
void xhci_doorbell_control(xhci_device* dev)
{
    *(uint32_t*)(((uint8_t*)dev->base) + dev->cap_regs->dboff) = 0;
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
    // dev->op_regs->usbcmd |= USBCMD_RESET;
    // while (dev->op_regs->usbcmd & USBCMD_RESET)
    //     OBOSS_SpinlockHint();
    OBOS_Log("XHCI: Reset XHCI controller at %02x:%02x:%02x\n", dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function);
    return OBOS_STATUS_SUCCESS;
}

bool poll_bit_timeout(volatile uint32_t *field, uint32_t mask, uint32_t expected, uint32_t us_timeout)
{
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(us_timeout);
    while ((*field & mask) != expected)
    {
        if (CoreS_GetTimerTick() >= deadline)
            return false;
        OBOSS_SpinlockHint();
    }
    return true;
}