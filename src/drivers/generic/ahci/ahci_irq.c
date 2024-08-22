/*
 * drivers/generic/ahci/ahci_irq.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>

#include <locks/event.h>
#include <locks/semaphore.h>

#include "ahci_irq.h"
#include "structs.h"
#include "command.h"

// static dpc ahci_dpc;
// static void ahci_dpc_handler(dpc* d, void* userdata)
// {
//     OBOS_UNUSED(d);
//     OBOS_UNUSED(userdata);
    
// }
bool ahci_irq_checker(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(userdata);
    return HBA->is != 0;
}
irq HbaIrq;
void ahci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(oldIrql);
    OBOS_Debug("AHCI IRQ.\n");
        HBA->is = HBA->is;
    for (uint8_t i = 0; i < 32; i++)
    {
        if (!(HBA->is & BIT(i)))
            continue;
        Port* curr = &Ports[i];
        if (!curr->hbaPort)
        {
            HBA->ports[i].is = HBA->ports[i].is;
            continue;
        }
        OBOS_Debug("Port %d IRQ.\n", i);
        uint32_t portStatus = curr->hbaPort->is;
        if (!portStatus)
            continue;
        curr->hbaPort->is = portStatus;
        // Signal each finished event here.
        obos_status status = OBOS_STATUS_SUCCESS;
        if (portStatus & (0xFD800000))
        {
            // Some command failed.
            // (How sad)
            // Signal all commands with OBOS_STATUS_RETRY.

            status = OBOS_STATUS_RETRY;
        }
        for (uint8_t slot = 0; slot < HBA->cap.nsc; slot++)
        {
            if ((curr->hbaPort->ci & BIT(slot)) && status == OBOS_STATUS_SUCCESS)
                continue;
            if (!curr->PendingCommands[slot])
                continue; // There was never a command issued in the first place.
            OBOS_Debug("Slot %d IRQ.\n", slot);
            curr->PendingCommands[slot]->commandStatus = status;
            Core_EventSet(&curr->PendingCommands[slot]->completionEvent, false);
            Core_SemaphoreRelease(&curr->lock);
        }
    }
    // CoreH_InitializeDPC(&ahci_dpc, ahci_dpc_handler, Core_DefaultThreadAffinity);
}