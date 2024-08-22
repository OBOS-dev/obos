/*
 * drivers/generic/ahci/command.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <locks/semaphore.h>

#include "structs.h"
#include "command.h"

obos_status SendCommand(Port* port, struct command_data* data, uint64_t lba, uint8_t device, uint16_t count)
{
    if (!port || !data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (data->physRegionCount > 32)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Core_SemaphoreAcquire(&port->lock);
    StopCommandEngine(port->hbaPort);
    // uint32_t cmdSlot = 31;
    uint32_t cmdSlot = __builtin_ctz(~(port->hbaPort->ci | port->hbaPort->sact));
    port->PendingCommands[cmdSlot] = data;
    obos_status status = OBOS_STATUS_SUCCESS;
    HBA_CMD_HEADER* cmdHeader = ((HBA_CMD_HEADER*)port->clBase) + cmdSlot;
    HBA_CMD_TBL* cmdTBL = 
    (HBA_CMD_TBL*)
    (
        (uintptr_t)port->clBase +
            ((cmdHeader->ctba | ((uintptr_t)cmdHeader->ctbau << 32)) - port->clBasePhys) // The offset of the HBA_CMD_TBL
    );
    cmdHeader->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t);
    if (data->direction == COMMAND_DIRECTION_READ)
        cmdHeader->w = 0;   // Device to Host.
    else
        cmdHeader->w = 1;   // Host to Device.
    cmdHeader->prdtl = data->physRegionCount;
    for (size_t i = 0; i < data->physRegionCount; i++)
    {
        if (!HBA->cap.s64a)
            OBOS_ASSERT(!(data->phys_regions[i].phys >> 32));
        memzero((void*)&cmdTBL->prdt_entry[i], sizeof(cmdTBL->prdt_entry[i]));
        AHCISetAddress(data->phys_regions[i].phys, cmdTBL->prdt_entry[i].dba);
        cmdTBL->prdt_entry[i].dbc = data->phys_regions[i].sz - 1;
        cmdTBL->prdt_entry[i].i = 1;
    }
    FIS_REG_H2D* fis = (void*)cmdTBL->cfis;
    memzero(fis, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->command = data->cmd;
    fis->device = device;
    fis->countl = count & 0xff;
    fis->counth = count >> 8;
    fis->lba0 = (lba & 0xff);
    fis->lba1 = (lba >> 8) & 0xff;
    fis->lba2 = (lba >> 16) & 0xff;
    fis->lba3 = (lba >> 24) & 0xff;
    fis->lba4 = (lba >> 32) & 0xff;
    fis->lba5 = (lba >> 40) & 0xff;
    fis->c = 1;
    // Wait for the port.
    // 0x88: ATA_DEV_BUSY | ATA_DEV_DRQ
    while ((port->hbaPort->tfd & 0x88))
        OBOSS_SpinlockHint();
    // Issue the command
    data->internal.cmdSlot = cmdSlot;
    StartCommandEngine(port->hbaPort);
    port->hbaPort->sact |= (1 << cmdSlot);
    port->hbaPort->ci |= (1 << cmdSlot);
    // Release the semaphore in the IRQ handler instead.
    // Core_SemaphoreRelease(&port->lock);
    return status;
}
void StopCommandEngine(volatile HBA_PORT* port)
{
    port->cmd &= ~(1<<0); // PxCMD.st

    // Clear FRE (bit 4)
    port->cmd &= ~(1<<4);

    // Wait until FR (bit 14), CR (bit 15) are cleared
    while (1)
    {
        if (port->cmd & (1<<14))
                continue;
        if (port->cmd & (1<<15))
                continue;
        break;
    }
}
void StartCommandEngine(volatile HBA_PORT* port)
{
    // Set FRE (bit 4) and ST (bit 0)
    port->cmd |= (1<<4);
    while (port->cmd & (1<<15))
        OBOSS_SpinlockHint();
    port->cmd |= (1<<0);
}