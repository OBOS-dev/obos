/*
 * drivers/generic/ehci/ehci_irq.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irq.h>

#include "structs.h"

bool ehci_irq_check(irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    ehci_controller* controller = userdata;
    controller->usbsts |= (controller->op_base_reg->usbsts & 0x3f);
    controller->op_base_reg->usbsts = controller->op_base_reg->usbsts & 0x3f;
    return controller->usbsts != 0;
}
void ehci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && userdata && oldIrql);
    OBOS_Warning("%s: Unimplemented\n", __func__);
}