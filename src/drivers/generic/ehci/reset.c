/*
 * drivers/generic/ehci/reset.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <driver_interface/pci.h>

#include <utils/list.h>

#include "irq/timer.h"
#include "mm/pmm.h"
#include "structs.h"

ehci_controllers g_controllers;
size_t g_controller_count;

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

void ehci_initialize_controller(ehci_controller* controller)
{
    // for (volatile bool b = true; b; )
    //     ;

    OBOS_ENSURE(controller);
    OBOS_ENSURE(!controller->initialized);

    // Map the BARs.
    pci_bar* bar = nullptr;
    pci_resource* irq_resource = nullptr;
    for (pci_resource* res = LIST_GET_HEAD(pci_resource_list, &controller->dev->resources); res; )
    {
        if (res->type == PCI_RESOURCE_BAR && res->bar->idx == 0)
        {
            bar = res->bar;
            controller->bar_resource = res;
        }
        else if (res->type == PCI_RESOURCE_IRQ)
            irq_resource = controller->irq_resource = res;

        if (irq_resource && bar)
            break;

        res = LIST_GET_NEXT(pci_resource_list, &controller->dev->resources, res);
    }

    Core_IrqObjectInitializeIRQL(&controller->irq, IRQL_EHCI, true, true);
    
    irq_resource->irq->irq = &controller->irq;
    irq_resource->irq->masked = false;
    Drv_PCISetResource(irq_resource);
    
    controller->irq.irqCheckerUserdata = controller;
    controller->irq.handlerUserdata = controller;
    controller->irq.irqChecker = ehci_irq_check;
    controller->irq.handler = ehci_irq_handler;

    controller->bar_base = map_registers(bar->phys, bar->size, true);
    controller->op_base_reg = (void*)((char*)controller->bar_base + controller->base_reg->caplength);

    controller->initialized = true;

    OBOS_Log("EHCI: %02x:%02x:%02x: Initialized EHCI controller. BAR at 0x%p, IRQ on vector ID 0x%x\n",
        controller->dev->location.bus,controller->dev->location.slot,controller->dev->location.function,
        controller->bar_resource->bar->phys, controller->irq.vector->id);

    controller->periodicList.phys = ehci_alloc_phys(OBOS_PAGE_SIZE <= (4*1024) ? OBOS_PAGE_SIZE/(4*1024) : 1);
    controller->periodicList.virt = map_registers(controller->periodicList.phys, (OBOS_PAGE_SIZE <= (4*1024) ? OBOS_PAGE_SIZE/(4*1024) : 1)*OBOS_PAGE_SIZE, false);
    for (size_t i = 0; i < 1024; i++)
        controller->periodicList.virt[i] |= BIT(0);

    ehci_reset_controller(controller);
}

void ehci_reset_controller(ehci_controller* controller)
{
    // Halt the controler.
    controller->op_base_reg->usbcmd &= ~BIT(0);
    if (!wait_bit_timeout(&controller->op_base_reg->usbsts, BIT(12), BIT(12), 2000))
        return;
    controller->op_base_reg->usbcmd |= BIT(1); // Reset
    if (!wait_bit_timeout(&controller->op_base_reg->usbcmd, BIT(1), 0, 2000 /* 16 microframes */))
        return;
    controller->op_base_reg->ctrldsssegment = 0; // All allocated memory is in the bottom 4GiB segment
    controller->op_base_reg->usbintr = 0; // enable all IRQs
    controller->op_base_reg->periodiclistbase = controller->periodicList.phys;
}

uintptr_t ehci_alloc_phys(size_t nPages)
{
    return Mm_AllocatePhysicalPages32(nPages, 1, nullptr);
}

bool wait_bit_timeout(volatile uint32_t* field, uint32_t mask, uint32_t expected, uint32_t us_timeout)
{
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(us_timeout);
    while (((*field) & mask) != expected)
    {
        if (deadline <= CoreS_GetTimerTick())
            return false;
        OBOSS_SpinlockHint();
    }
    return true;
}