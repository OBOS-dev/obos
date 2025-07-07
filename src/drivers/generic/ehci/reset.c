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

#include <allocators/base.h>

#include <irq/timer.h>
#include <irq/irq.h>
#include <irq/irql.h>

#include "error.h"
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

    controller->dev->resource_cmd_register->cmd_register |= (BIT(1)|BIT(2));
    controller->dev->resource_cmd_register->cmd_register &= ~BIT(10);
    Drv_PCISetResource(controller->dev->resource_cmd_register);

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

    controller->bar_base = map_registers(bar->phys, bar->size, true);
    controller->op_base_reg = (void*)((char*)controller->bar_base + controller->base_reg->caplength);

    controller->nPorts = controller->base_reg->hcsparams & 0xf;
    controller->ports = ZeroAllocate(OBOS_KernelAllocator, controller->nPorts, sizeof(ehci_port), nullptr);
    controller->debug_port = (controller->base_reg->hcsparams >> 20) & 0xf;
    controller->debug_port_ptr = controller->debug_port ? &controller->ports[controller->debug_port-1] : nullptr;
    for (size_t i = 0; i < controller->nPorts; i++)
    {
        controller->ports[i].id = i+1;
        controller->ports[i].sc = &controller->op_base_reg->portsc[i];
    }

    irq_resource->irq->irq = &controller->irq;
    irq_resource->irq->masked = false;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Drv_PCISetResource(irq_resource)))
    {
        OBOS_Error("EHCI: %02x:%02x:%02x: Drv_PCISetResource(irq_resource) returned status %d\n",
            controller->dev->location.bus,controller->dev->location.slot,controller->dev->location.function, 
            status);
            return;
    }
    
    controller->irq.irqCheckerUserdata = controller;
    controller->irq.handlerUserdata = controller;
    controller->irq.irqChecker = ehci_irq_check;
    controller->irq.handler = ehci_irq_handler;

    controller->eecp = (controller->base_reg->hccparams >> 8) & 0xff;

    OBOS_Log("EHCI: %02x:%02x:%02x: Initialized EHCI controller. BAR at 0x%p, IRQ on vector ID 0x%x. Controller has %d ports\n",
        controller->dev->location.bus,controller->dev->location.slot,controller->dev->location.function,
        controller->bar_resource->bar->phys, controller->irq.vector->id,
        controller->nPorts);
    if (controller->debug_port)
        OBOS_Log("EHCI: %02x:%02x:%02x: NOTE: Debug port on port %d\n",
            controller->dev->location.bus,controller->dev->location.slot,controller->dev->location.function,
            controller->debug_port
        );

    controller->initialized = true;
    
    if (!ehci_do_bios_handoff(controller))
    {
        OBOS_Error("EHCI: %02x:%02x:%02x: Could not do BIOS handoff: Timed out.\n",
            controller->dev->location.bus,controller->dev->location.slot,controller->dev->location.function
        );
        return;
    }

    ehci_reset_controller(controller);
}

bool ehci_do_bios_handoff(ehci_controller* controller)
{
    OBOS_ASSERT(!controller->bios_handoff_done);
    
    uint64_t usblegsup = 0;
    DrvS_ReadPCIRegister(controller->dev->location, controller->eecp, 4, &usblegsup);
    usblegsup |= BIT(24); // OS Owned Semaphore
    DrvS_WritePCIRegister(controller->dev->location, controller->eecp, 4, usblegsup);
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(60*1000*1000);
    do {
        DrvS_ReadPCIRegister(controller->dev->location, controller->eecp, 4, &usblegsup);
        if (CoreS_GetTimerTick() >= deadline)
            return false;
    } while(usblegsup & BIT(16) /* BIOS Owner Semaphore*/);

    return (controller->bios_handoff_done = true);
}

void ehci_reset_controller(ehci_controller* controller)
{
    OBOS_ENSURE(controller->initialized);

    // Halt the controler.
    controller->op_base_reg->usbcmd &= ~BIT(0);
    if (!wait_bit_timeout(&controller->op_base_reg->usbsts, BIT(12), BIT(12), 2050 /* 16 microframes */))
    {
        OBOS_Debug("usbsts&0x1000 == 0x1000 timed after 2ms\n");
        return;
    }

    controller->op_base_reg->usbcmd |= BIT(1); // Reset
    if (!wait_bit_timeout(&controller->op_base_reg->usbcmd, BIT(1), 0, UINT32_MAX /* an unrealistically long time for this to complete*/))
    {
        OBOS_Debug("usbcmd&0x2 == 0x0 timed after 1.1 hours\n");
        return;
    }
    
    controller->op_base_reg->ctrldsssegment = 0; // All allocated memory is in the bottom 4GiB segment

    uint32_t usbcmd = controller->op_base_reg->usbcmd;
    usbcmd &= ~(0xff<<16);
    usbcmd |= BIT(19);
    usbcmd &= ~(BIT(4)|BIT(5));
    usbcmd |= BIT(0); // Start the controller.
    controller->op_base_reg->usbcmd = usbcmd;
    wait_bit_timeout(&controller->op_base_reg->usbsts, BIT(12), 0, UINT32_MAX);
    
    controller->op_base_reg->configflag = 1;

    controller->op_base_reg->usbintr = 0x37; // enable most IRQs
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