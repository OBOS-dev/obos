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
static obos_status unpopulate_physical_regions(uintptr_t base, size_t size, struct command_data* data, bool wasPageable, bool wasUC);
#pragma GCC push_options
#pragma GCC optimize ("-O0")
static obos_status populate_physical_regions(uintptr_t base, size_t size, struct command_data* data, bool* wasPageable, bool *wasUC)
{
    bool tried = false;
    page what = { .addr=(base - (base%OBOS_PAGE_SIZE)) };
    try_again:
    OBOS_ASSERT(data);
    context* volatile context = CoreS_GetCPULocalPtr()->currentContext;
    page* volatile found = RB_NFIND(page_tree, &context->pages, &what);
    if (!found)
    {
        if (OBOS_PAGE_SIZE == OBOS_HUGE_PAGE_SIZE || tried)
            return OBOS_STATUS_NOT_FOUND;
        tried = true;
        what.addr -= (what.addr % OBOS_HUGE_PAGE_SIZE);
        goto try_again;
    }
    if (found->addr != what.addr) // the address is the key
        found = RB_PREV(page_tree, &context->pages, found);
    *wasPageable = found->pageable;
    *wasUC = found->prot.uc;
    obos_status status = Mm_VirtualMemoryProtect(CoreS_GetCPULocalPtr()->currentContext, (void*)(base - base % OBOS_PAGE_SIZE), size, OBOS_PROTECTION_SAME_AS_BEFORE|OBOS_PROTECTION_CACHE_DISABLE, 0);
    if (obos_is_error(status))
        return status;
    base &= ~1;
    const size_t MAX_PRDT_COUNT = sizeof(((HBA_CMD_TBL*)nullptr))->prdt_entry/sizeof(HBA_PRDT_ENTRY);
    uintptr_t reg_base = 0;
    uintptr_t physical_page = 0;
    uintptr_t prev_phys = 0;
    int64_t reg_size = 0;
    int64_t pg_size = 0;
    int64_t bytesLeft = size;
    int64_t bytesInPage = 0;
    // int64_t initialBytesInPage = 0;
    bool trunc = false;
    for (uintptr_t addr = base; bytesLeft && (addr == base || (bytesInPage > 0)); addr += bytesInPage)
    {
        // if (data->physRegionCount == 38)
        //     for (volatile bool b = true; b; );
        found = RB_NEXT(page_tree, &context->pages, found);
        pg_size = found->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        if (data->physRegionCount >= MAX_PRDT_COUNT)
        {
            unpopulate_physical_regions(base, size, data, *wasPageable, *wasUC);
            return OBOS_STATUS_INTERNAL_ERROR;
        }
        OBOSS_GetPagePhysicalAddress((void*)addr, &physical_page);
        physical_page += addr % pg_size;
        if (bytesLeft < pg_size)
            bytesInPage = bytesLeft;
        else
            bytesInPage = pg_size - (addr % pg_size);
        if (((addr + (bytesInPage - 1)) & ~(OBOS_PAGE_SIZE-1)) != (addr & ~(OBOS_PAGE_SIZE-1)))
        {
            trunc = true;
            bytesInPage = pg_size - (addr % pg_size);
        }
        else
            trunc = false;
        if (addr == base)
        {
            // initialBytesInPage = bytesInPage;
            reg_base = trunc ? 0 : physical_page;
        }
        // What does this even do?!?!?!?!??!?!
        // if (bytesLeft <= pg_size && ((int64_t)size) > pg_size)
        //     bytesInPage -= initialBytesInPage;
        if ((addr != base) && (prev_phys + pg_size) == physical_page && reg_size < (1024*1024*4 /* Hard limit imposed by AHCI */))
        {
            reg_size += (bytesLeft > bytesInPage ? bytesInPage : bytesLeft);
        }
        else
        {
            if (addr != base || ((int64_t)size) <= pg_size)
            {
                data->phys_regions = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, data->phys_regions, ++data->physRegionCount*sizeof(struct ahci_phys_region), nullptr);
                struct ahci_phys_region* reg = &data->phys_regions[data->physRegionCount - 1];
                reg->phys = reg_base ? reg_base : physical_page;
                reg->sz = reg_size ? reg_size : (bytesLeft > bytesInPage ? bytesInPage : bytesLeft);
            }
            reg_base = !trunc ? physical_page : 0;
            reg_size = !trunc ? (bytesLeft > bytesInPage ? bytesInPage : bytesLeft) : 0;
        }
        prev_phys = physical_page;
        bytesLeft -= bytesInPage;
    }
    if (!data->physRegionCount || reg_size > 0)
    {
        data->phys_regions = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, data->phys_regions, ++data->physRegionCount*sizeof(struct ahci_phys_region), nullptr);
        struct ahci_phys_region* reg = &data->phys_regions[data->physRegionCount - 1];
        reg->phys = reg_base;
        reg->sz = reg_size;
    }
    return OBOS_STATUS_SUCCESS;
}
static obos_status unpopulate_physical_regions(uintptr_t base, size_t size, struct command_data* data, bool wasPageable, bool wasUC)
{
    OBOS_ASSERT(data);
    obos_status status = Mm_VirtualMemoryProtect(CoreS_GetCPULocalPtr()->currentContext, (void*)base, size, OBOS_PROTECTION_SAME_AS_BEFORE|(!wasUC ? OBOS_PROTECTION_CACHE_ENABLE : OBOS_PROTECTION_CACHE_DISABLE), wasPageable);
    if (obos_is_error(status))
        return status;
    return OBOS_STATUS_SUCCESS;
}
#pragma GCC pop_options
obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // FIXME: Allow for reads greater than 0x10000 sectors.
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
    if ((blkOffset + blkCount) > port->nSectors)
        blkCount = (blkOffset + blkCount) - port->nSectors;
    if (!blkCount)
    {
        if (nBlkRead)
            *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    obos_status status = OBOS_STATUS_SUCCESS;
    struct command_data data = { .direction=COMMAND_DIRECTION_READ, .cmd=(port->supports48bitLBA ? ATA_READ_DMA_EXT : ATA_READ_DMA ) };
    bool wasPageable = false;
    bool wasUC = false;
    data.completionEvent = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    status = populate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, &wasPageable, &wasUC);
    if (obos_is_error(status))
        return status;
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
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, wasPageable, wasUC);
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
    unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, wasPageable, wasUC);
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
    bool wasPageable = false;
    bool wasUC = false;
    status = populate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, &wasPageable, &wasUC);
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
            unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, wasPageable, wasUC);
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
    unpopulate_physical_regions((uintptr_t)buf, blkCount*port->sectorSize, &data, wasPageable, wasUC);
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