/*
 * drivers/x86_64/xhci/init.c
 *
 * Copyright (c) 2025 Omar Berrow
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
    if (dev->pci_bar->bar->iospace == PCI_BARIO)
        return OBOS_STATUS_INTERNAL_ERROR; // this shouldn't happen

    OBOS_Log("XHCI: Initializing XHCI controller at %02x:%02x:%02x\n", dev->dev->location.bus, dev->dev->location.slot, dev->dev->location.function);

    dev->base = map_registers(dev->pci_bar->bar->phys, dev->pci_bar->bar->size, false);
    dev->op_regs = (void*)((uintptr_t)dev->base + dev->cap_regs->caplength);

    Core_IrqObjectInitializeIRQL(&dev->irq, IRQL_XHCI, true, true);
    dev->irq.irqChecker = xhci_irq_checker;
    dev->irq.handler = xhci_irq_handler;
    dev->irq.handlerUserdata = dev;
    dev->irq.irqCheckerUserdata = dev;
    dev->pci_irq->irq->masked = false;
    dev->pci_irq->irq->irq = &dev->irq;
    Drv_PCISetResource(dev->pci_irq);
    dev->irq.irqChecker = xhci_irq_checker;
    dev->irq.handler = xhci_irq_handler;
    dev->irq.handlerUserdata = dev;
    dev->irq.irqCheckerUserdata = dev;

    return OBOS_STATUS_SUCCESS;
}

obos_status xhci_reset_device(xhci_device* dev);