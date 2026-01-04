/*
 * oboskrnl/mm/swap.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
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
#include <vfs/irp.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <irq/irql.h>

swap_dev* Mm_SwapProvider;

static thread page_writer_thread;
static _Atomic(size_t) page_writer_waiters = 0;
static event page_writer_wake = EVENT_INITIALIZE(EVENT_SYNC);
static event page_writer_done = EVENT_INITIALIZE(EVENT_SYNC);
static mutex swap_lock = MUTEX_INITIALIZE();

obos_status Mm_SwapOut(uintptr_t virt, context* ctx)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!ctx)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // OBOS_ASSERT(!page->reserved);
    // if (page->reserved)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    uintptr_t phys = 0;
    page_info page = {};
    obos_status status = MmS_QueryPageInfo(ctx->pt, virt, &page, &phys);
    if (obos_is_error(status))
        return status;
    if (page.prot.is_swap_phys)
        return OBOS_STATUS_SUCCESS;

    struct page key = { .phys = page.phys };
    Core_MutexAcquire(&Mm_PhysicalPagesLock);
    struct page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &key);
    Core_MutexRelease(&Mm_PhysicalPagesLock);
    if (!pg)
    {
        OBOS_Warning("%s: Could not find 'struct page' for physical page 0x%p\n", __func__, page.phys);
        return OBOS_STATUS_INTERNAL_ERROR;
    }
    
    uintptr_t swap_id = 0;
    irql oldIrql = IRQL_INVALID;
    if (ctx == &Mm_KernelContext && Core_SpinlockAcquired(&Mm_KernelContext.lock))
    {
        oldIrql = Core_GetIrql();
        Core_SpinlockRelease(&Mm_KernelContext.lock, IRQL_DISPATCH);
    }
    status = Mm_SwapProvider->swap_resv(Mm_SwapProvider, &swap_id, page.prot.huge_page);
    if (oldIrql != IRQL_INVALID)
        oldIrql > Core_GetIrql() ? oldIrql = Core_RaiseIrql(oldIrql) : Core_LowerIrql(oldIrql);
    if (obos_is_error(status))
        return status;

    swap_allocation* swap_alloc = MmH_AddSwapAllocation(swap_id);
    MmH_RefSwapAllocation(swap_alloc);
    swap_alloc->phys = pg;
    MmH_RefPage(pg);
    pg->swap_alloc = swap_alloc;

    page.prot.present = false;
    page.prot.is_swap_phys = true;
    page.phys = swap_id;
    phys = swap_id;
    status = MmS_SetPageMapping(ctx->pt, &page, phys, false);

    MmS_TLBShootdown(ctx->pt, page.virt, page.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    if (obos_is_error(status))
    {
        OBOS_Warning("%s: MmS_SetPageMapping returned %d\n", __func__, status);
        return status;
    }
    if (page.dirty)
        Mm_MarkAsDirtyPhys(pg);
    else
        Mm_MarkAsStandbyPhys(pg);
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_SwapIn(page_info* page, fault_type* type)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->prot.is_swap_phys)
        OBOS_ASSERT(page->phys);
    // OBOS_ASSERT(!page->reserved);
    // if (page->reserved)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->range->pageable)
        return OBOS_STATUS_UNPAGED_POOL;
    irql oldIrql = Mm_TakeSwapLock();
    page_info arch_pg_info = {.virt=page->virt};
    MmS_QueryPageInfo(page->range->ctx->pt, arch_pg_info.virt, &arch_pg_info, nullptr);
    if (arch_pg_info.prot.is_swap_phys)
        goto down;
    struct page what = {.phys=page->phys};
    Core_MutexAcquire(&Mm_PhysicalPagesLock);
    struct page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Core_MutexRelease(&Mm_PhysicalPagesLock);
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
        // else
        //     OBOS_ASSERT(!"Funny business");
        node->pagedCount++;
        page->prot.present = true;
        obos_status status = MmS_SetPageMapping(page->range->ctx->pt, page, node->phys, false);
        // Free(Mm_Allocator, node, sizeof(*node));
        if (obos_expect(obos_is_error(status), false))
        {
            // Unlikely error.
            Mm_ReleaseSwapLock(oldIrql);
            return status;
        }
        Mm_ReleaseSwapLock(oldIrql);
        if (type)
            *type = SOFT_FAULT;
        return OBOS_STATUS_SUCCESS;
    }
    else 
    {
        // Not swapped out.
        Mm_ReleaseSwapLock(oldIrql);
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
        Mm_ReleaseSwapLock(oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    if (!alloc->phys)
    {
        alloc->phys = MmH_PgAllocatePhysical(page->range->phys32, page->range->prot.huge_page);
        if (obos_is_error(alloc->provider->swap_read(alloc->provider, alloc->id, alloc->phys)))
        {
            Mm_ReleaseSwapLock(oldIrql);
            if (type)
                *type = ACCESS_FAULT;
            return OBOS_STATUS_SUCCESS;
        }
    }
    else
        MmH_RefPage(alloc->phys);
    if (alloc->phys->flags & PHYS_PAGE_STANDBY)
        LIST_REMOVE(phys_page_list, &Mm_StandbyPageList, alloc->phys);
    else if (alloc->phys->flags & PHYS_PAGE_DIRTY)
    {
        if (!alloc->phys->pagedCount)
            LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, alloc->phys);
        Mm_DirtyPagesBytes -= page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }
    phys = alloc->phys->phys;
    if (page->range)
        page->prot = page->range->prot;
    page->prot.present = true;
    page->prot.is_swap_phys = false;
    page->phys = phys;
    alloc->phys->pagedCount++;
    status = MmS_SetPageMapping(page->range->ctx->pt, page, phys, false);
    if (obos_is_error(status))
    {
        Mm_FreePhysicalPages(phys, nPages);
        Mm_ReleaseSwapLock(oldIrql);
        return status;
    }
    MmH_DerefSwapAllocation(alloc);
    Mm_ReleaseSwapLock(oldIrql);
    if (type)
        *type = HARD_FAULT;
    return OBOS_STATUS_SUCCESS;
}

obos_status Mm_ChangeSwapProvider(swap_dev* to)
{
    Mm_SwapProvider->awaiting_deinit = true;
    Mm_SwapProvider = to;
    return OBOS_STATUS_SUCCESS;
}

uint32_t Mm_PageWriterOperation = 0;

static __attribute__((no_instrument_function)) void page_writer()
{
    // const char* const This = "Page Writer";
    while (1)
    {
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(page_writer_wake));
        OBOS_ENSURE(obos_is_success(status));
        Core_EventClear(&page_writer_done);
        if (!Mm_PageWriterOperation)
            Mm_PageWriterOperation = PAGE_WRITER_SYNC_ALL;
        // FOR EACH dirty page.
        // Write them back :)
        // also while we're at it, we'll make them standby
        irql oldIrql = Mm_TakeSwapLock();
        for (page* pg = LIST_GET_HEAD(phys_page_list, &Mm_DirtyPageList); pg && (Mm_PageWriterOperation & PAGE_WRITER_SYNC_ANON); )
        {
            page* next = LIST_GET_NEXT(phys_page_list, &Mm_DirtyPageList, pg);
            if (next == pg)
                next = nullptr;
            // OBOS_ENSURE(pg->flags & PHYS_PAGE_DIRTY);
            if (~pg->flags & PHYS_PAGE_DIRTY)
            {
                // Funny business
                LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, pg);
                pg = next;
                continue;
            }
            if (!pg->backing_vn)
            {
                Mm_ReleaseSwapLock(oldIrql);
                if (obos_is_error(Mm_SwapProvider->swap_write(Mm_SwapProvider, pg->swap_alloc->id, pg)))
                {
                    pg->swap_alloc->refs--;
                    MmH_DerefSwapAllocation(pg->swap_alloc);
                    pg = next;
                    continue;
                }
                Mm_GlobalMemoryUsage.paged += pg->flags & PHYS_PAGE_HUGE_PAGE ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
                oldIrql = Mm_TakeSwapLock();
                pg->flags &= ~PHYS_PAGE_DIRTY;
                LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, pg);

                LIST_APPEND(phys_page_list, &Mm_StandbyPageList, pg);
                pg->flags |= PHYS_PAGE_STANDBY;
            }

            // abort:
            pg = next;
        }

        for (page* pg = LIST_GET_HEAD(phys_page_list, &Mm_DirtyPageList); pg && (Mm_PageWriterOperation & PAGE_WRITER_SYNC_FILE); )
        {
            page* next = LIST_GET_NEXT(phys_page_list, &Mm_DirtyPageList, pg);
            if (next == pg)
                next = nullptr;
            // OBOS_ENSURE(pg->flags & PHYS_PAGE_DIRTY);
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
                size_t nBytes = pg->end_offset - pg->file_offset;
                OBOS_ENSURE(nBytes <= OBOS_PAGE_SIZE);
                driver_header* driver = Vfs_GetVnodeDriver(pg->backing_vn);
                if (!driver)
                    continue;
                size_t blkSize = 0;
                driver->ftable.get_blk_size(pg->backing_vn->desc, &blkSize);
                nBytes /= blkSize;
                const size_t base_offset = pg->backing_vn->flags & VFLAGS_PARTITION ? (pg->backing_vn->partitions[0].off/blkSize) : 0;
                const uintptr_t offset = (pg->file_offset / pg->backing_vn->blkSize) + base_offset;
                // if (!VfsH_LockMountpoint(point))
                //     goto abort;
                Mm_ReleaseSwapLock(oldIrql);
                // printf("writing back %p:%d (real offset: %d)\n", pg->backing_vn, pg->file_offset, offset);
                obos_status status = driver->ftable.write_sync(pg->backing_vn->desc, MmS_MapVirtFromPhys(pg->phys), nBytes, offset, nullptr);
                if (obos_is_error(status))
                    OBOS_Error("I/O Error while flushing page. Status: %d\n", status);
                oldIrql = Mm_TakeSwapLock();
                // VfsH_UnlockMountpoint(point);
                pg->flags &= ~PHYS_PAGE_DIRTY;
                LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, pg);
        
                LIST_APPEND(phys_page_list, &Mm_StandbyPageList, pg);
                pg->flags |= PHYS_PAGE_STANDBY;
        }
            
            // abort:
            pg = next;
        }

        Mm_DirtyPagesBytes = 0;
        Mm_ReleaseSwapLock(oldIrql);
        Core_EventSet(&page_writer_done, false);
    }
}

phys_page_list Mm_DirtyPageList;
phys_page_list Mm_StandbyPageList;
size_t Mm_DirtyPagesBytes;
size_t Mm_DirtyPagesBytesThreshold = OBOS_PAGE_SIZE * 128;

size_t Mm_CachedBytes;

void Mm_MarkAsDirty(page_info* pg)
{
    OBOS_ASSERT(pg->phys);
    if (pg->prot.is_swap_phys)
        return;
    page what = {.phys=pg->phys};
    Core_MutexAcquire(&Mm_PhysicalPagesLock);
    page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Core_MutexRelease(&Mm_PhysicalPagesLock);
    Mm_MarkAsDirtyPhys(node);
}

void Mm_MarkAsStandby(page_info* pg)
{
    if (pg->prot.is_swap_phys)
        return;
    page what = {.phys=pg->phys};
    Core_MutexAcquire(&Mm_PhysicalPagesLock);
    page* node = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Core_MutexRelease(&Mm_PhysicalPagesLock);
    Mm_MarkAsStandbyPhys(node);
}

void Mm_MarkAsDirtyPhys(page* node)
{
    OBOS_ASSERT(node);

    // NOTE: While this might seem like a fatal/impossible condition,
    // stuff like fbdev use the pagecache to allow for mapping of the
    // framebuffer from userspace.
    if (node->flags & PHYS_PAGE_MMIO)
        return;

    if (node->flags & PHYS_PAGE_DIRTY)
        return;
 
    irql oldIrql = Mm_TakeSwapLock();
    node->flags |= PHYS_PAGE_DIRTY;
    if (node->flags & PHYS_PAGE_STANDBY)
        LIST_REMOVE(phys_page_list, &Mm_StandbyPageList, node);
    node->flags &= ~PHYS_PAGE_STANDBY;
    MmH_RefPage(node);
    LIST_APPEND(phys_page_list, &Mm_DirtyPageList, node);
    // if (node->backing_vn)
    //     printf("%p:%d\n", node->backing_vn, node->file_offset);
    Mm_DirtyPagesBytes += (node->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    Mm_ReleaseSwapLock(oldIrql);
    Mm_PageWriterOperation |= PAGE_WRITER_SYNC_ANON;
    if (Mm_DirtyPagesBytes > Mm_DirtyPagesBytesThreshold && !node->backing_vn)
        Mm_WakePageWriter(false);
}

void Mm_MarkAsStandbyPhys(page* node)
{
    OBOS_ASSERT(node);
    if (node->flags & PHYS_PAGE_STANDBY)
        return;
    // See note for Mm_MarkAsDirtyPhys for why this is done.
    if (node->flags & PHYS_PAGE_MMIO)
        return;

    irql oldIrql = Mm_TakeSwapLock();

    if (node->flags & PHYS_PAGE_DIRTY)
    {
        LIST_REMOVE(phys_page_list, &Mm_DirtyPageList, node);
        Mm_DirtyPagesBytes -= (node->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }
    
    MmH_RefPage(node);
    LIST_APPEND(phys_page_list, &Mm_StandbyPageList, node);
    node->flags |= PHYS_PAGE_STANDBY;
    Mm_ReleaseSwapLock(oldIrql);
    if (node->flags & PHYS_PAGE_DIRTY)
    {
        node->flags &= ~PHYS_PAGE_DIRTY;
        MmH_DerefPage(node);
    }
}

void Mm_InitializePageWriter()
{
    // page_writer_wake = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    // page_writer_done = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(&ctx, (uintptr_t)page_writer, 0, false,  Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x20000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 0x20000);
    CoreH_ThreadInitialize(&page_writer_thread, THREAD_PRIORITY_LOW, Core_DefaultThreadAffinity, &ctx);
    CoreH_ThreadReady(&page_writer_thread);
}

// Wakes up the page writer to free up memory
// Set 'wait' to true to wait for the page writer to release
void Mm_WakePageWriter(bool wait)
{
    OBOS_ASSERT(page_writer_thread.status);
    Core_EventPulse(&page_writer_wake, true);
    obos_status status = OBOS_STATUS_SUCCESS;
    if (wait)
    {
        page_writer_waiters++;
        status = Core_WaitOnObject(WAITABLE_OBJECT(page_writer_done));
        if (!(--page_writer_waiters))
            Core_EventClear(&page_writer_done);
    }
    OBOS_UNUSED(status);
}

irql Mm_TakeSwapLock()
{
    Core_MutexAcquire(&swap_lock);
    return IRQL_INVALID;
}

void Mm_ReleaseSwapLock(irql oldIrql)
{
    OBOS_UNUSED(oldIrql);
    Core_MutexRelease(&swap_lock);
    // Core_SpinlockRelease(&swap_lock, oldIrql);
}
