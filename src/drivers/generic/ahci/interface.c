/*
 * drivers/generic/ahci/interface.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <driver_interface/header.h>

#include <mm/alloc.h>
#include <mm/page.h>
#include <mm/context.h>
#include <mm/bare_map.h>

#include <locks/event.h>
#include <locks/wait.h>
#include <locks/semaphore.h>

#include <irq/irql.h>

#include <scheduler/cpu_local.h>

#include <utils/tree.h>

#include <allocators/base.h>

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
#pragma GCC push_options
#pragma GCC optimize ("-O0")
static OBOS_NO_KASAN void page_in(uintptr_t base, size_t sz)
{
    uintptr_t curr_base = base;
    for (size_t i = 0; i < sz; i += OBOS_PAGE_SIZE)
    {
        volatile char prev = ((char*)curr_base)[i];
        ((volatile char*)curr_base)[i] = prev;
    }
}
static obos_status populate_physical_regions(uintptr_t base, size_t size, struct command_data* data)
{
    // obos_status status = 
    //     *wasPageable ?
    //         Mm_VirtualMemoryProtect(CoreS_GetCPULocalPtr()->currentContext, (void*)(base - base % OBOS_PAGE_SIZE), size, OBOS_PROTECTION_SAME_AS_BEFORE|OBOS_PROTECTION_CACHE_DISABLE, 0) :
    //         OBOS_STATUS_SUCCESS;
    // if (obos_is_error(status))
    //     return status;
    page_in(base, size);
    base &= ~1;
    const size_t MAX_PRDT_COUNT = sizeof(((HBA_CMD_TBL*)nullptr))->prdt_entry/sizeof(HBA_PRDT_ENTRY);
/*
    data->phys_regions = Mm_Allocator->Reallocate(Mm_Allocator, data->phys_regions, ++data->physRegionCount*sizeof(struct ahci_phys_region), nullptr);
    struct ahci_phys_region* reg = &data->phys_regions[data->physRegionCount - 1];
*/
    long bytesLeft = size;
    size_t pgSize = 0;
    uintptr_t lastPhys = 0;
    struct ahci_phys_region curr = {};
    bool wroteback = false;
    for (uintptr_t addr = base; bytesLeft >= 0; bytesLeft -= (pgSize-(addr%pgSize)), addr += (pgSize-(addr%pgSize)))
    {
        if (data->physRegionCount >= MAX_PRDT_COUNT)
            return OBOS_STATUS_INTERNAL_ERROR;
        page_info info = {};
        MmS_QueryPageInfo(CoreS_GetCPULocalPtr()->currentContext->pt, addr, &info, nullptr);
        if ((lastPhys + pgSize) != info.phys)
        {
            if (addr != base)
            {
                size_t old_sz = data->physRegionCount*sizeof(struct ahci_phys_region);
                data->phys_regions = Mm_Allocator->Reallocate(Mm_Allocator, data->phys_regions, ++data->physRegionCount*sizeof(struct ahci_phys_region), old_sz, nullptr);
                struct ahci_phys_region* reg = &data->phys_regions[data->physRegionCount - 1];
                *reg = curr;
                wroteback = true;
            }
            pgSize = (info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
            curr.phys = info.phys + (addr % pgSize);
            curr.sz = 0;
        }
        pgSize = (info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        size_t addend = (bytesLeft > (long)pgSize ? pgSize : (size_t)bytesLeft);
        curr.sz += addend;
        if (curr.sz > (pgSize-(addr % pgSize)))
        {
            curr.sz -= addend;
            curr.sz += pgSize-(addr % pgSize);
        }
        lastPhys = info.phys;
        wroteback = false;
    }
    if (!wroteback && curr.phys)
    {
        size_t old_sz = data->physRegionCount*sizeof(struct ahci_phys_region);
        data->phys_regions = Mm_Allocator->Reallocate(Mm_Allocator, data->phys_regions, ++data->physRegionCount*sizeof(struct ahci_phys_region), old_sz, nullptr);
        struct ahci_phys_region* reg = &data->phys_regions[data->physRegionCount - 1];
        *reg = curr;
    }
    return OBOS_STATUS_SUCCESS;
}
static obos_status unpopulate_physical_regions(uintptr_t base, size_t size, struct command_data* data)
{
    OBOS_ASSERT(data);
    OBOS_ASSERT(base);
    OBOS_ASSERT(size);
    // obos_status status =
    //     wasPageable ?
    //         Mm_VirtualMemoryProtect(CoreS_GetCPULocalPtr()->currentContext, (void*)base, size, OBOS_PROTECTION_SAME_AS_BEFORE|(!wasUC ? OBOS_PROTECTION_CACHE_ENABLE : OBOS_PROTECTION_CACHE_DISABLE), true) :
    //         OBOS_STATUS_SUCCESS;
    // if (obos_is_error(status))
    //     return status;
    return OBOS_STATUS_SUCCESS;
}
#pragma GCC pop_options
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
        SendCommand(port, &data, blkOffset, 0x40, blkCount == 0x10000 ? 0 : blkCount);
        // Core_LowerIrql(oldIrql);
        Core_WaitOnObject(WAITABLE_OBJECT(data.completionEvent));
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
        Core_WaitOnObject(WAITABLE_OBJECT(data.completionEvent));
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
