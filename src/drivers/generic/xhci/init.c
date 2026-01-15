/*
 * drivers/x86_64/xhci/init.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <driver_interface/pci.h>
#include <driver_interface/header.h>

#include <allocators/base.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>
#include <irq/timer.h>

#include <mm/context.h>
#include <mm/alloc.h>

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

    dev->dev->resource_cmd_register->cmd_register |= 0x6;
    Drv_PCISetResource(dev->dev->resource_cmd_register);

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

    uint32_t hccparams1 = dev->cap_regs->hccparams1;
    dev->has_64bit_support = hccparams1 & BIT(0);
    dev->port_power_control_supported = hccparams1 & BIT(3);
    dev->xecp = (hccparams1 >> 16)*4;

    obos_status status = xhci_reset_device(dev);
    if (obos_is_error(status))
    {
        dev->pci_irq->irq->masked = false;
        dev->pci_irq->irq->irq = nullptr;
        Drv_PCISetResource(dev->pci_irq);
        Core_IrqObjectFree(&dev->irq);
    }

    return status;
}

bool xhci_irq_checker(irq* i, void* udata) { OBOS_UNUSED(i && udata); printf("sus\n"); return false; }
void xhci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql) { OBOS_UNUSED(i && frame && userdata && oldIrql); return; }

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

obos_status xhci_reset_device(xhci_device* dev)
{
    OBOS_ENSURE(dev);
    if (!dev)
        return OBOS_STATUS_INVALID_ARGUMENT;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (!dev->did_bios_handoff)
        if (obos_is_error(status = do_bios_handoff(dev)))
            return status;
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