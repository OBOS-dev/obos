/*
 * drivers/generic/ahci/interface.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <driver_interface/header.h>

#include <mm/alloc.h>
#include <mm/page.h>
#include <mm/context.h>
#include <mm/handler.h>
#include <mm/bare_map.h>

#include <locks/event.h>
#include <locks/wait.h>
#include <locks/semaphore.h>

#include <irq/irql.h>

#include <scheduler/cpu_local.h>

#include <utils/tree.h>

#include <allocators/base.h>

#include <vfs/irp.h>

#include "command.h"
#include "structs.h"

obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    if (!desc || !blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Port* port = (Port*)desc;
    *blkSize = port->sectorSize;
    return OBOS_STATUS_SUCCESS;
}
obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!desc || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Port* port = (Port*)desc;
    *count = port->nSectors;
    return OBOS_STATUS_SUCCESS;
}
static obos_status unpopulate_physical_regions(uintptr_t base, size_t size, struct command_data* data);
// #pragma GCC push_options
// #pragma GCC optimize ("-O0")
static obos_status populate_physical_regions(uintptr_t base, size_t size, struct command_data* data)
{
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
#ifndef __x86_64__
    if (base >= OBOS_KERNEL_ADDRESS_SPACE_BASE && (base + size) < OBOS_KERNEL_ADDRESS_SPACE_LIMIT)
        ctx = &Mm_KernelContext;
#else
    if (base >= 0xffff800000000000 && (base + size) < OBOS_KERNEL_ADDRESS_SPACE_LIMIT)
        ctx = &Mm_KernelContext;
#endif
    const size_t MAX_PRDT_COUNT = sizeof(((HBA_CMD_TBL*)nullptr))->prdt_entry/sizeof(HBA_PRDT_ENTRY);

    return DrvH_ScatterGather(
        ctx, 
        (void*)base, size,
        &data->phys_regions, &data->physRegionCount,
        MAX_PRDT_COUNT,
        data->direction == COMMAND_DIRECTION_READ ? true : false);
}
static obos_status unpopulate_physical_regions(uintptr_t base, size_t size, struct command_data* data)
{
    OBOS_ASSERT(data);
    OBOS_ASSERT(base);
    OBOS_ASSERT(size);
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
#ifndef __x86_64__
    if (base >= OBOS_KERNEL_ADDRESS_SPACE_BASE && (base + size) < OBOS_KERNEL_ADDRESS_SPACE_LIMIT)
        ctx = &Mm_KernelContext;
#else
    if (base >= 0xffff800000000000 && (base + size) < OBOS_KERNEL_ADDRESS_SPACE_LIMIT)
        ctx = &Mm_KernelContext;
#endif
    
    obos_status status = DrvH_FreeScatterGatherList(ctx, (void*)base, size, data->phys_regions, data->physRegionCount);
    data->phys_regions = nullptr;

    return status;
}
// #pragma GCC pop_options
obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (blkCount > 0x10000)
    {
        size_t nSectorBlocks = blkCount / 0x10000;
        size_t leftOver = blkCount % 0x10000;
        obos_status status = OBOS_STATUS_SUCCESS;
        if (nBlkRead)
            *nBlkRead = 0;
        for (size_t i = 0; i < nSectorBlocks; i++)
        {
            size_t read = 0;
            if (obos_is_error(status = read_sync(desc, (void*)((uintptr_t)buf + i*0x10000), 0x10000, i*0x10000, &read)))
                return status;
            if (nBlkRead)
                *nBlkRead += read;
        }
        if (leftOver)
        {
            size_t read = 0;
            if (obos_is_error(status = read_sync(desc, (void*)((uintptr_t)buf + nSectorBlocks*0x10000), 0x10000, leftOver, &read)))
                return status;
            if (nBlkRead)
                *nBlkRead += read;
        }
        return status;
    }
    Port* port = (Port*)desc;
    if (!port->works)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        return OBOS_STATUS_ABORTED;
    }
    if (blkCount > port->nSectors)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    if (blkOffset > port->nSectors)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((blkOffset + blkCount) > port->nSectors)
        blkCount = blkOffset - port->nSectors;
    if (!blkCount)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    struct command_data data = { .direction=COMMAND_DIRECTION_READ, .cmd=(port->supports48bitLBA ? ATA_READ_DMA_EXT : ATA_READ_DMA ) };
    data.completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    status = populate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
    if (obos_is_error(status))
        return status;
    // for (size_t i = 0; i < data.physRegionCount; i++)
    //     printf("%d: 0x%p 0x%08x\n", i, data.phys_regions[i].phys, data.phys_regions[i].sz);
    for (uint8_t try = 0; try < 5; try++)
    {
        HBA->ghc |= BIT(1);
        // irql oldIrql = Core_RaiseIrql(IRQL_AHCI);
        if (obos_is_error(status = SendCommand(port, &data, blkOffset, 0x40, blkCount == 0x10000 ? 0 : blkCount)))
        {
            Core_SemaphoreRelease(&port->lock);
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
            break;
        }
        // Core_LowerIrql(oldIrql);
        if (obos_is_error(status = Core_WaitOnObject(WAITABLE_OBJECT(data.completionEvent))))
        {
            Core_SemaphoreRelease(&port->lock);
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
            break;
        }
        Core_EventClear(&data.completionEvent);
        if (!port->works)
        {
            Core_SemaphoreRelease(&port->lock);
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
            status = OBOS_STATUS_ABORTED; // oops
            break;
        }
        if (obos_is_success(data.commandStatus))
            break;
        if (obos_is_error(data.commandStatus) && data.commandStatus != OBOS_STATUS_RETRY)
        {
            status = data.commandStatus;
            break;
        }
    }
    unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
    if (nBlkRead)
        *nBlkRead = blkCount;
    return status;
}
obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // FIXME: Allow for writes greater than 0x10000 sectors.
    if (blkCount > 0x10000)
        blkCount = 0x10000;
    Port* port = (Port*)desc;
    if (!port->works)
    {
        if (nBlkWritten)
            *nBlkWritten = 0;
        return OBOS_STATUS_ABORTED;
    }
    if (blkCount > port->nSectors)
    {
        if (nBlkWritten)
            *nBlkWritten = 0;
        return OBOS_STATUS_SUCCESS;
    }
    if ((blkOffset + blkCount) > port->nSectors)
        blkCount = (blkOffset + blkCount) - port->nSectors;
    if (!blkCount)
    {
        if (nBlkWritten)
            *nBlkWritten = 0;
        return OBOS_STATUS_SUCCESS;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    struct command_data data = { .direction=COMMAND_DIRECTION_WRITE, .cmd=(port->supports48bitLBA ? ATA_WRITE_DMA_EXT : ATA_WRITE_DMA ) };
    data.completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    status = populate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
    if (obos_is_error(status))
        return status;
    for (uint8_t try = 0; try < 5; try++)
    {
        SendCommand(port, &data, blkOffset, 0x40, blkCount == 0x10000 ? 0 : blkCount);
        HBA->ghc |= BIT(1) /* GhcIE */;
        if (obos_is_error(status = Core_WaitOnObject(WAITABLE_OBJECT(data.completionEvent))))
        {
            Core_SemaphoreRelease(&port->lock);
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
            break;
        }
        Core_EventClear(&data.completionEvent);
        if (!port->works)
        {
            Core_SemaphoreRelease(&port->lock);
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data);
            status = OBOS_STATUS_ABORTED; // oops
            break;
        }
        if (obos_is_success(data.commandStatus))
            break;
        if (obos_is_error(data.commandStatus) && data.commandStatus != OBOS_STATUS_RETRY)
        {
            status = data.commandStatus;
            break;
        }
    }
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    return status;
}
obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u)
{
    if (!cb)
        return OBOS_STATUS_INVALID_ARGUMENT;
    for (uint8_t port = 0; port < 32; port++)
        cb((dev_desc)(Ports + port), Ports[port].sectorSize, Ports[port].nSectors, u);
    return OBOS_STATUS_SUCCESS;
}
obos_status query_user_readable_name(dev_desc desc, const char** name)
{
    if (!desc || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Port* port = (Port*)desc;
    *name = port->dev_name;
    return OBOS_STATUS_SUCCESS;
}

obos_status submit_irp(void* request_)
{
    if (!request_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* request = request_;
    Port* port = (Port*)request->desc;
    if (!port || !request->buff || !request->refs)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!port->works)
    {
        VfsH_IRPSignal(request, OBOS_STATUS_ABORTED);
        return OBOS_STATUS_SUCCESS;
    }
    if (request->blkCount > port->nSectors)
    {
        request->nBlkRead = 0;
        VfsH_IRPSignal(request, OBOS_STATUS_SUCCESS);
        return OBOS_STATUS_SUCCESS;
    }
    if (request->blkOffset > port->nSectors)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((request->blkOffset + request->blkCount) > port->nSectors)
        request->blkCount = request->blkOffset - port->nSectors;
    if (!request->blkCount)
    {
        request->nBlkRead = 0;
        VfsH_IRPSignal(request, OBOS_STATUS_SUCCESS);
        return OBOS_STATUS_SUCCESS;
    }
    if (request->dryOp)
    {
        // Assume the AHCI driver can always do I/O.
        // This is probably true, considering there are 32 command slots
        // and not enough disk I/O done in the kernel to exhaust that.
        VfsH_IRPSignal(request, OBOS_STATUS_SUCCESS);
        return OBOS_STATUS_SUCCESS;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    struct command_data *data = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(struct command_data), nullptr);
    switch (request->op) {
        case IRP_READ:
            data->cmd = port->supports48bitLBA ? ATA_READ_DMA_EXT : ATA_READ_DMA;
            data->direction = COMMAND_DIRECTION_READ;
            break;
        case IRP_WRITE:
            data->cmd = port->supports48bitLBA ? ATA_WRITE_DMA_EXT : ATA_WRITE_DMA;
            data->direction = COMMAND_DIRECTION_WRITE;
            break;
    }
    data->completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    request->evnt = &data->completionEvent;
    request->drvData = data;
    status = populate_physical_regions((uintptr_t)request->buff, request->blkCount*port->sectorSize, data);
    if (obos_is_error(status))
    {
        Free(OBOS_NonPagedPoolAllocator, data, sizeof(struct command_data));
        request->drvData = nullptr;
        VfsH_IRPSignal(request, status);
        return OBOS_STATUS_SUCCESS;
    }
    SendCommand(port, data, request->blkOffset, 0x40, request->blkCount == 0x10000 ? 0 : request->blkCount);
    HBA->ghc |= BIT(1) /* GhcIE */;
    return OBOS_STATUS_SUCCESS;
}
obos_status finalize_irp(void* request_)
{
    if (!request_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* request = request_;
    if (!request->drvData)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct command_data* data = request->drvData;
    if (obos_is_success(data->commandStatus))
        request->nBlkRead = request->blkCount;
    else
        request->nBlkRead = 0;
    unpopulate_physical_regions((uintptr_t)request->buff, request->blkCount, data);
    Free(OBOS_NonPagedPoolAllocator, data, sizeof(struct command_data));
    request->drvData = nullptr;
    return OBOS_STATUS_SUCCESS;
}