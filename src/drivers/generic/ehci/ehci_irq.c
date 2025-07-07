/*
 * drivers/generic/ehci/ehci_irq.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irq.h>
#include <irq/dpc.h>

#include "structs.h"

static void ehci_dpc(dpc* unused, void* userdata)
{
    OBOS_UNUSED(unused);
    ehci_controller* controller = userdata;
    if (controller->usbsts & BIT(2))
    {
        for (size_t i = 0; i < controller->nPorts; i++)
            // most everything is handled in the USB stack, the job of the driver is to signal.
            if (*controller->ports[i].sc & BIT(1))
                ehci_signal_connection_change(controller, &controller->ports[i], *controller->ports[i].sc & BIT(0));     
    }
    controller->usbsts = 0; 
}

bool ehci_irq_check(irq* i, void* userdata)
{
    if (!userdata)
        return false;
    OBOS_UNUSED(i);
    ehci_controller* controller = userdata;
    controller->usbsts |= (controller->op_base_reg->usbsts & 0x3f);
    controller->op_base_reg->usbsts = controller->op_base_reg->usbsts & 0x3f;
    return controller->usbsts != 0;
}
void ehci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_ENSURE_NPANIC(userdata);
    if (!userdata)
        return;
    OBOS_UNUSED(i && frame && oldIrql);
    ehci_controller* controller = userdata;
    controller->exec_dpc.userdata = userdata;
    CoreH_InitializeDPC(&controller->exec_dpc, ehci_dpc, 0);
}