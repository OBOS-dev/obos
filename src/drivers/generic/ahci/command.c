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
#include <locks/mutex.h>

#include <irq/timer.h>

#include "locks/wait.h"
#include "structs.h"
#include "command.h"

static _Atomic(bool) transactions_halted = false;
obos_status SendCommand(Port* port, struct command_data* data, uint64_t lba, uint8_t device, uint16_t count)
{
    if (!port || !data)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (data->physRegionCount > sizeof(((HBA_CMD_TBL*)nullptr))->prdt_entry/sizeof(HBA_PRDT_ENTRY))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (transactions_halted)
        return OBOS_STATUS_RETRY;
    StopCommandEngine(&HBA->ports[port->hbaPortIndex]);
    Core_SemaphoreAcquire(&port->lock);
    HBA->ports[port->hbaPortIndex].is = 0xffffffff;
    Core_MutexAcquire(&port->bitmask_lock);
    uint32_t cmdSlot = __builtin_ctz(~port->CommandBitmask);
    data->internal.cmdSlot = cmdSlot;
    port->CommandBitmask |= BIT(cmdSlot);
    port->PendingCommands[cmdSlot] = data;
    Core_MutexRelease(&port->bitmask_lock);
    obos_status status = OBOS_STATUS_SUCCESS;
    volatile HBA_CMD_HEADER* cmdHeader = ((HBA_CMD_HEADER*)port->clBase) + cmdSlot;
    cmdHeader->b0 |= ((sizeof(FIS_REG_H2D) / sizeof(uint32_t)) & 0x1f) << 0;
    if (data->direction == COMMAND_DIRECTION_READ)
        cmdHeader->b0 &= ~BIT(6);   // Device to Host.
    else
        cmdHeader->b0 |= BIT(6);   // Host to Device.
    // cmdHeader->b1 |= BIT(2);
#if OBOS_ARCHITECTURE_BITS == 64
    volatile HBA_CMD_TBL* cmdTBL = 
    (HBA_CMD_TBL*)(uintptr_t)
    (
        (uintptr_t)port->clBase +
            ((cmdHeader->ctba | ((uint64_t)cmdHeader->ctbau << 32)) - port->clBasePhys) // The offset of the HBA_CMD_TBL
    );
#else
    HBA_CMD_TBL* cmdTBL = 
    (HBA_CMD_TBL*)(uintptr_t)
    (
        (uintptr_t)port->clBase +
            ((cmdHeader->ctba - port->clBasePhys)) // The offset of the HBA_CMD_TBL
    );
#endif
    memzero((void*)cmdTBL, sizeof(*cmdTBL));
    for (uint16_t i = 0; i < data->physRegionCount; i++)
    {
    #if OBOS_ARCHITECTURE_BITS == 64
        if (!(HBA->cap & BIT(31)))
            OBOS_ASSERT(!(data->phys_regions[i].phys >> 32));
    #endif
        memzero((void*)&cmdTBL->prdt_entry[i], sizeof(cmdTBL->prdt_entry[i]));
        AHCISetAddress(data->phys_regions[i].phys, cmdTBL->prdt_entry[i].dba);
        uint32_t dw4 = ((data->phys_regions[i].sz - 1) & 0x3fffff) << 0;
        // cmdTBL->prdt_entry[i].i = (i == (data->physRegionCount - 1));
        // cmdTBL->prdt_entry[i].dw4 &= ~BIT(31);
        dw4 |= BIT(31);
        cmdTBL->prdt_entry[i].dw4 = dw4;
    }
    cmdHeader->prdtl = data->physRegionCount;
    FIS_REG_H2D* fis = (void*)cmdTBL->cfis;
    memzero(fis, sizeof(*fis));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->b1 |= BIT(7);
    fis->command = data->cmd;

    fis->lba0 = (lba & 0xff);
    fis->lba1 = (lba >> 8) & 0xff;
    fis->lba2 = (lba >> 16) & 0xff;
    fis->device = device;
    
    fis->lba3 = (lba >> 24) & 0xff;
    fis->lba4 = (lba >> 32) & 0xff;
    fis->lba5 = (lba >> 40) & 0xff;
    
    fis->countl = count & 0xff;
    fis->counth = count >> 8;
    // Wait for the port.
    // 0x88: ATA_DEV_BUSY | ATA_DEV_DRQ
    while ((HBA->ports[port->hbaPortIndex].tfd & 0x88))
        OBOSS_SpinlockHint();
    // Issue the command
    data->internal.cmdSlot = cmdSlot;
    // HBA->ports[port->hbaPortIndex].sact |= (1 << cmdSlot);
    StartCommandEngine(&HBA->ports[port->hbaPortIndex]);
    HBA->ports[port->hbaPortIndex].ci |= (1 << cmdSlot);
    // Release the semaphore in the IRQ handler instead.
    // Core_SemaphoreRelease(&port->lock);
    return status;
}
obos_status ClearCommand(Port* port, struct command_data* data)
{
    uint8_t cmdSlot = data->internal.cmdSlot;
    HBA->ports[port->hbaPortIndex].ci &= ~(1 << cmdSlot);
    port->PendingCommands[cmdSlot] = nullptr;
    return OBOS_STATUS_SUCCESS;
}
void StopCommandEngine(volatile HBA_PORT* hPort)
{
    //  PxCMD.ST: Bit 0
    // PxCMD.FRE: Bit 4
    //  PxCMD.FR: Bit 14
    //  PxCMD.CR: Bit 15
    if (!(hPort->cmd & (BIT(0) | BIT(4) | BIT(14) | BIT(15))))
        return;
    // The DMA engine is running.
    // Disable it.
    hPort->cmd &= ~(1<<0);
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(3000000 /* 1s */);
    while(hPort->cmd & (1<<15) /*PxCMD.CR*/ && deadline > CoreS_GetTimerTick())
        OBOSS_SpinlockHint();
    if (hPort->cmd & (1<<15) /*PxCMD.CR*/)
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Port did not go idle after 3 seconds (PxCMD.CR=1). PxCMD: 0x%08x\n", hPort->cmd);
    hPort->cmd &= ~(1<<4);
    deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(3000000);
    while(hPort->cmd & (1<<14) /*PxCMD.FR*/ && deadline > CoreS_GetTimerTick())
        OBOSS_SpinlockHint();
    if (hPort->cmd & (1<<14) /*PxCMD.FR*/)
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Port did not go idle after 3 seconds (PxCMD.FR=1). PxCMD: 0x%08x\n", hPort->cmd);
}
void StartCommandEngine(volatile HBA_PORT* port)
{
    port->cmd |= (1<<4);
    while (port->cmd & (1<<15))
        OBOSS_SpinlockHint();
    port->cmd |= (1<<0);
}

void HaltTranscations()
{
    transactions_halted = true;
}
void ResumeTranscations()
{
    transactions_halted = false;
}

void WaitForTranscations()
{
    for (size_t i = 0; i < PortCount; i++)
    {
        Port* port = Ports + i;
        for (size_t j = 0; j < 32; j++)
            if (port->CommandBitmask & BIT(j))
                Core_WaitOnObject(WAITABLE_OBJECT(port->PendingCommands[j]->completionEvent));
    }
}
