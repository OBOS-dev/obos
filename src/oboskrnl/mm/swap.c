/*
 * oboskrnl/mm/swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <partition.h>
#include <error.h>

#include <mm/bare_map.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/alloc.h>
#include <mm/handler.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <locks/event.h>
#include <locks/spinlock.h>
#include <locks/wait.h>
#include <locks/mutex.h>

#include <vfs/mount.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <irq/irql.h>

swap_dev* Mm_SwapProvider;

static thread page_writer_thread;
static thread_node page_writer_thread_node;
static event page_writer_wake = EVENT_INITIALIZE(EVENT_SYNC);
static event page_writer_done = EVENT_INITIALIZE(EVENT_SYNC);
static spinlock swap_lock;

obos_status Mm_SwapOut(uintptr_t virt, page_range* rng)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!rng)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // OBOS_ASSERT(!page->reserved);
    // if (page->reserved)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(rng->pageable);
    if (!rng->pageable)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uintptr_t phys = 0;
    page_info page = {};
    obos_status status = MmS_QueryPageInfo(rng->ctx->pt, virt, &page, &phys);
    if (obos_is_error(status))
        return status;
    page.prot.present = false;
    status = MmS_SetPageMapping(rng->ctx->pt, &page, phys, false);
    if (obos_is_error(status))
        return status;
    if (page.dirty)
        Mm_MarkAsDirty(&page);
    else
        Mm_MarkAsStandby(&page);
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_SwapIn(page_info* page, fault_type* type)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(page->phys);
    // OBOS_ASSERT(!page->reserved);
    // if (page->reserved)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->range->pageable)
        return OBOS_STATUS_UNPAGED_POOL;
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    page_info arch_pg_info = {.virt=page->virt};
    MmS_QueryPageInfo(page->range->ctx->pt, arch_pg_info.virt, &arch_pg_info, nullptr);
    if (arch_pg_info.prot.is_swap_phys)
        goto down;
    struct page what = {.phys=page->phys};
    struct page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    const bool onDirtyList = (node->flags & PHYS_PAGE_DIRTY);
    const bool onStandbyList = (node->flags & PHYS_PAGE_STANDBY);
    if ((onDirtyList||onStandbyList))
    {
        // Simply remap the page.
        if (onDirtyList)
        {
            if (!node->pagedCount)
                LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, node);
            Mm_DirtyPagesBytes -= page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        }
        else if (onStandbyList && !node->pagedCount)
            LIST_REMOVE(phys_page_list, &Mm_StandbyPageList, node);
        MmH_DerefPage(node);
        // else
        //     OBOS_ASSERT(!"Funny business");
        node->pagedCount++;
        page->prot.present = true;
        obos_status status = MmS_SetPageMapping(page->range->ctx->pt, page, node->phys, false);
        // Mm_Allocator->Free(Mm_Allocator, node, sizeof(*node));
        if (obos_expect(obos_is_error(status), false))
        {
            // Unlikely error.
            Core_SpinlockRelease(&swap_lock, oldIrql);
            return status;
        }
        Core_SpinlockRelease(&swap_lock, oldIrql);
        if (type)
            *type = SOFT_FAULT;
        return OBOS_STATUS_SUCCESS;
    }
    else 
    {
        // Not swapped out.
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND; 
    }
    down:
    OBOS_UNUSED(0);
    size_t nPages = (page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE;
    obos_status status = OBOS_STATUS_SUCCESS;
    uintptr_t phys = 0;
    swap_allocation* alloc = MmH_LookupSwapAllocation(page->phys);
    if (!alloc)
    {
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    if (!alloc->phys)
    {
        alloc->phys = MmH_PgAllocatePhysical(page->range->phys32, page->range->prot.huge_page);
        if (obos_is_error(alloc->provider->swap_read(alloc->provider, alloc->id, alloc->phys)))
        {
            Core_SpinlockRelease(&swap_lock, oldIrql);
            if (type)
                *type = ACCESS_FAULT /* maybe don't do this? TODO */;
            return OBOS_STATUS_INTERNAL_ERROR;
        }
    }
    phys = alloc->phys->phys;
    page->prot.present = true;
    page->phys = phys;
    status = MmS_SetPageMapping(page->range->ctx->pt, page, phys, false);
    if (obos_is_error(status))
    {
        Mm_FreePhysicalPages(phys, nPages);
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return status;
    }
    Core_SpinlockRelease(&swap_lock, oldIrql);
    if (type)
        *type = HARD_FAULT;
    return OBOS_STATUS_SUCCESS;
}

obos_status Mm_ChangeSwapProvider(swap_dev* to)
{
    OBOS_UNUSED(to);
    return OBOS_STATUS_UNIMPLEMENTED;
}

static __attribute__((no_instrument_function)) void page_writer()
{
    // const char* const This = "Page Writer";
    while (1)
    {
        Core_WaitOnObject(WAITABLE_OBJECT(page_writer_wake));
        Core_EventClear(&page_writer_done);
        // FOR EACH dirty page.
        // Write them back :)
        // also while we're at it, we'll make them standby
        irql oldIrql = Core_SpinlockAcquire(&swap_lock);
        for (page* pg = LIST_GET_HEAD(phys_page_list, &Mm_DirtyPageList); pg; )
        {
            page* next = LIST_GET_NEXT(phys_page_list, &Mm_DirtyPageList, pg);
            if (~pg->flags & PHYS_PAGE_DIRTY)
            {
                // Funny business
                LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, pg);
                pg = next;
                continue;
            }
            if (pg->backing_vn)
            {
                // This is a file page, so writing it back is different than writing back an
                // anonymous page.
                size_t nBytes = OBOS_PAGE_SIZE;
                mount* const point = pg->backing_vn->mount_point ? pg->backing_vn->mount_point : pg->backing_vn->un.mounted;
                const driver_header* driver = pg->backing_vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
                if (pg->backing_vn->vtype == VNODE_TYPE_BLK)
                    driver = &pg->backing_vn->un.device->driver->header;
                size_t blkSize = 0;
                driver->ftable.get_blk_size(pg->backing_vn->desc, &blkSize);
                nBytes /= blkSize;
                const size_t base_offset = pg->backing_vn->flags & VFLAGS_PARTITION ? (pg->backing_vn->partitions[0].off/blkSize) : 0;
                const uintptr_t offset = pg->file_offset + base_offset;
                // if (!VfsH_LockMountpoint(point))
                //     goto abort;
                Core_SpinlockRelease(&swap_lock, oldIrql);
                obos_status status = driver->ftable.write_sync(pg->backing_vn->desc, MmS_MapVirtFromPhys(pg->phys), nBytes, offset, nullptr);
                if (obos_is_error(status))
                    OBOS_Error("I/O Error while flushing page. Status: %d\n", status);
                oldIrql = Core_SpinlockAcquire(&swap_lock);
                // VfsH_UnlockMountpoint(point);
            }
            else 
            {
                if (obos_is_error(Mm_SwapProvider->swap_resv(Mm_SwapProvider, &pg->swap_id, pg->flags & PHYS_PAGE_HUGE_PAGE)))
                {
                    pg = next;
                    continue;
                }
                swap_allocation* alloc = MmH_AddSwapAllocation(pg->swap_id);
                alloc->refs = pg->virt_pages.nNodes;
                if (obos_is_error(Mm_SwapProvider->swap_write(Mm_SwapProvider, pg->swap_id, pg)))
                {
                    alloc->refs = 1;
                    MmH_DerefSwapAllocation(alloc);
                    pg = next;
                    continue;
                }
            }
            pg->flags &= ~PHYS_PAGE_DIRTY;
            LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, pg);
            Mm_DirtyPagesBytes -= (pg->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    
            LIST_APPEND(phys_page_list, &Mm_StandbyPageList, pg);
            pg->flags |= PHYS_PAGE_STANDBY;
            
            // abort:
            pg = next;
        }
        Core_SpinlockRelease(&swap_lock, oldIrql);
        Core_EventSet(&page_writer_done, false);
    }
}

phys_page_list Mm_DirtyPageList;
phys_page_list Mm_StandbyPageList;
size_t Mm_DirtyPagesBytes;
size_t Mm_DirtyPagesBytesThreshold = OBOS_PAGE_SIZE * 50;
void Mm_MarkAsDirty(page_info* pg)
{
    OBOS_ASSERT(pg->phys);
    page what = {.phys=pg->phys};
    page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Mm_MarkAsDirtyPhys(node);
}

void Mm_MarkAsStandby(page_info* pg)
{
    page what = {.phys=pg->phys};
    page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Mm_MarkAsStandbyPhys(node);
}

void Mm_MarkAsDirtyPhys(page* node)
{
    OBOS_ASSERT(node);

    if (node->pagedCount && --node->pagedCount)
    {
        node->flags |= PHYS_PAGE_DIRTY;
        return;
    }
 
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    node->flags |= PHYS_PAGE_DIRTY;
    node->flags &= ~PHYS_PAGE_STANDBY;
    MmH_RefPage(node);
    LIST_APPEND(phys_page_list, &Mm_DirtyPageList, node);
    Mm_DirtyPagesBytes += (node->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    Core_SpinlockRelease(&swap_lock, oldIrql);
    if (Mm_DirtyPagesBytes > Mm_DirtyPagesBytesThreshold)
        Mm_WakePageWriter(false);
}

void Mm_MarkAsStandbyPhys(page* node)
{
    OBOS_ASSERT(node);
    if (node->flags & PHYS_PAGE_STANDBY)
        return;
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);

    if (node->pagedCount && --node->pagedCount)
    {
        node->flags |= PHYS_PAGE_STANDBY;
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return;
    }

    if (node->flags & PHYS_PAGE_DIRTY)
    {
        LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, node);
        Mm_DirtyPagesBytes -= (node->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }
    
    MmH_RefPage(node);
    LIST_APPEND(phys_page_list, &Mm_StandbyPageList, node);
    node->flags |= PHYS_PAGE_STANDBY;
    Core_SpinlockRelease(&swap_lock, oldIrql);
}

void Mm_InitializePageWriter()
{
    // page_writer_wake = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    // page_writer_done = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(&ctx, (uintptr_t)page_writer, 0, false,  Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x20000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 0x20000);
    CoreH_ThreadInitialize(&page_writer_thread, THREAD_PRIORITY_LOW, Core_DefaultThreadAffinity, &ctx);
    CoreH_ThreadReadyNode(&page_writer_thread, &page_writer_thread_node);
}

// Wakes up the page writer to free up memory
// Set 'wait' to true to wait for the page writer to release
void Mm_WakePageWriter(bool wait)
{
    OBOS_ASSERT(page_writer_thread.status);
    Core_EventPulse(&page_writer_wake, true);
    if (wait)
        Core_WaitOnObject(WAITABLE_OBJECT(page_writer_done));
}

irql Mm_TakeSwapLock()
{
    return Core_SpinlockAcquire(&swap_lock);
}

void Mm_ReleaseSwapLock(irql oldIrql)
{
    Core_SpinlockRelease(&swap_lock, oldIrql);
}
