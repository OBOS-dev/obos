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

#include <vfs/irp.h>

#include <locks/event.h>
#include <locks/semaphore.h>

#include <allocators/base.h>

#include "ahci_irq.h"
#include "structs.h"
#include "command.h"

// static dpc ahci_dpc;
static void ahci_dpc_handler(dpc* d, void* userdata)
{
    OBOS_UNUSED(d);
    OBOS_UNUSED(userdata);
    Port* curr = (Port*)userdata;
    for (uint8_t slot = 0; slot < (((HBA->cap >> 8) & 0b11111)+1); slot++)
    {
        if (!curr->PendingCommands[slot])
            continue; // There was never a command issued in the first place.
        if (curr->PendingCommands[slot]->awaitingSignal)
        {
            Core_SemaphoreRelease(&curr->lock);
            Core_EventSet(&curr->PendingCommands[slot]->completionEvent, false);
            curr->PendingCommands[slot]->awaitingSignal = false;
            curr->CommandBitmask &= ~BIT(slot);
            curr->PendingCommands[slot] = nullptr;
        }
    }
}
OBOS_NO_KASAN OBOS_NO_UBSAN bool ahci_irq_checker(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(userdata);
    // OBOS_Debug("bonjour HBA->is: 0x%08x\n", HBA->is);
    return HBA->is != 0;
}
irq HbaIrq;
OBOS_NO_KASAN OBOS_NO_UBSAN void ahci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(userdata);
    OBOS_UNUSED(oldIrql);
    // OBOS_Debug("hola\n");
    for (uint8_t port = 0; port < PortCount; port++)
    {
        Port* curr = &Ports[port];
        if (!(HBA->is & BIT(curr->hbaPortIndex)))
            continue;
        uint32_t portStatus = HBA->ports[curr->hbaPortIndex].is;
        if (!curr->works)
        {
            HBA->ports[curr->hbaPortIndex].is = portStatus;
            continue;
        }
        // Signal each finished event here.
        obos_status status = OBOS_STATUS_SUCCESS;
        if (portStatus & (0xFD800000))
        {
            // Some command failed.
            // (How sad)
            // Signal all commands with OBOS_STATUS_RETRY.

            status = OBOS_STATUS_RETRY;
        }
        bool requires_dpc = false;
        for (uint8_t slot = 0; slot < (((HBA->cap >> 8) & 0b11111)+1); slot++)
        {
            if ((HBA->ports[curr->hbaPortIndex].ci & BIT(slot)) && status == OBOS_STATUS_SUCCESS)
                continue;
            if (!curr->PendingCommands[slot])
                continue; // There was never a command issued in the first place.
            if (curr->PendingCommands[slot]->awaitingSignal)
                continue;
            // // TODO: Is this valid?
            // if (curr->PendingCommands[slot]->irp)
            //     curr->PendingCommands[slot]->irp->nBlkRead = (((HBA_CMD_HEADER*)curr->clBase) + slot)->prdbc / curr->sectorSize;
            curr->PendingCommands[slot]->awaitingSignal = true;
            requires_dpc = true;
            curr->PendingCommands[slot]->commandStatus = status;
        }
        if (requires_dpc)
        {
            curr->port_dpc.userdata = curr;
            CoreH_InitializeDPC(&curr->port_dpc, ahci_dpc_handler, Core_DefaultThreadAffinity);
        }
        HBA->ports[curr->hbaPortIndex].is = portStatus;
    }
    // Set HBA->is to itself to reset it.
    HBA->is = HBA->is;
}
