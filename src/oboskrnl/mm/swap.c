/*
 * oboskrnl/mm/swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <mm/bare_map.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/alloc.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <locks/event.h>
#include <locks/spinlock.h>
#include <locks/wait.h>
#include <locks/mutex.h>

#include <utils/tree.h>

#include <irq/irql.h>

swap_dev* Mm_SwapProvider;

static thread page_writer_thread;
static thread_node page_writer_thread_node;
static spinlock swap_lock;
static event page_writer_wake = EVENT_INITIALIZE(EVENT_SYNC);
static event page_writer_done = EVENT_INITIALIZE(EVENT_SYNC);

obos_status Mm_SwapOut(page* page)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(!page->reserved);
    if (page->reserved)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(!page->workingSets);
    if (!page->pageable || page->workingSets > 0)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uintptr_t phys = 0;
    obos_status status = MmS_QueryPageInfo(page->owner->pt, page->addr, nullptr, &phys);
    if (obos_is_error(status))
        return status;
    page->cached_phys = phys;
    page->prot.present = false;
    status = MmS_SetPageMapping(page->owner->pt, page, phys);
    if (obos_is_error(status))
    {
        page->prot.present = true;
        return status;
    }
    if (page->prot.dirty)
        Mm_MarkAsStandby(page);
    else
        Mm_MarkAsDirty(page);
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_SwapIn(page* page)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(!page->reserved);
    if (page->reserved)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->pageable)
        return OBOS_STATUS_SUCCESS;
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    if (!page->pagedOut && (page->onDirtyList||page->onStandbyList))
    {
        // Simply remap the page.
        if (page->onDirtyList)
        {
            REMOVE_PAGE_NODE(Mm_DirtyPageList, &page->ln_node);
            Mm_DirtyPagesBytes -= page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        }
        else if (page->onStandbyList)
            REMOVE_PAGE_NODE(Mm_StandbyPageList, &page->ln_node);
        else 
            OBOS_ASSERT(!"Funny business");
        page->onDirtyList = false;
        page->onStandbyList = false;
        page->prot.present = true;
        obos_status status = MmS_SetPageMapping(page->owner->pt, page, page->cached_phys);
        if (obos_expect(obos_is_error(status) == true, false))
        {
            // Unlikely error.
            Core_SpinlockRelease(&swap_lock, oldIrql);
            return status;
        }
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    size_t nPages = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    uintptr_t id = page->swapId;
    // Allocate a physical page.
    obos_status status = OBOS_STATUS_SUCCESS;
    uintptr_t phys = Mm_AllocatePhysicalPages(nPages, nPages, &status);
    if (obos_is_error(status))
    {
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return status;
    }
    // Read the page from the swap.
    status = Mm_SwapProvider->swap_read(Mm_SwapProvider, id, phys, nPages, page->swap_off);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_FreePhysicalPages(phys, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        Core_SpinlockRelease(&swap_lock, oldIrql);
        return status;
    }
    // status = Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages, page->swap_off);
    // if (obos_is_error(status) && status != OBOS_STATUS_NOT_FOUND)
    // {
    //     if (obos_is_error(Mm_FreePhysicalPages(phys, nPages)))
    //         return OBOS_STATUS_INTERNAL_ERROR;
    //     return status;
    // }
    // Re-map the page.
    page->prot.present = true;
    status = MmS_SetPageMapping(page->owner->pt, page, phys);
    if (obos_is_error(status)) // Give up if this fails.
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not remap page 0x%p. Status: %d.\n", page->addr, status);
    page->pagedOut = false;
    Core_SpinlockRelease(&swap_lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}

obos_status Mm_ChangeSwapProvider(swap_dev* to)
{
    if (!to)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!Mm_SwapProvider || to == Mm_SwapProvider)
    {
        Mm_SwapProvider = to;
        return OBOS_STATUS_SUCCESS;
    }
    page* curr = nullptr;
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    uintptr_t inter = Mm_AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE, 1, nullptr);
    RB_FOREACH(curr, page_tree, &Mm_KernelContext.pages)
    {
        if (!(curr->pagedOut||curr->onDirtyList||curr->onStandbyList))
            continue;
        if (curr->pagedOut)
            Mm_SwapProvider->swap_read(Mm_SwapProvider, curr->swapId, inter, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE, 0);
        Mm_SwapProvider->swap_free(Mm_SwapProvider, curr->swapId, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE, curr->swap_off);
        curr->swapId = 0;
        curr->swap_off = 0;
        uintptr_t id = 0;
        to->swap_resv(to, &id, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE);
        curr->swapId = id;
        if (curr->pagedOut)
            to->swap_write(to, curr->swapId, inter, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE, 0);
    }
    Mm_SwapProvider = to;
    Core_SpinlockRelease(&swap_lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static void mark_standby(page* pg);
static void page_writer()
{
    // const char* const This = "Page Writer";
    while (1)
    {
        Core_WaitOnObject(WAITABLE_OBJECT(page_writer_wake));
        // FOR EACH dirty page.
        // Write them back :)
        // also while we're at it, we'll make them standby
        uintptr_t swapId = 0;
        irql oldIrql = Core_SpinlockAcquire(&swap_lock);
        Mm_SwapProvider->swap_resv(Mm_SwapProvider, &swapId, Mm_DirtyPagesBytes);
        size_t currOffset = 0;
        // const size_t nReclaimed = Mm_DirtyPagesBytes;
        for (page_node* node = Mm_DirtyPageList.head; node; )
        {
            page* const curr = node->data; 
            node = node->next;
            // Write back
            curr->swap_off = currOffset;
            Mm_SwapProvider->swap_write(Mm_SwapProvider, swapId, curr->cached_phys, curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, currOffset);
            curr->owner->stat.paged += (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
            mark_standby(curr);
            currOffset += (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        }
        Core_SpinlockRelease(&swap_lock, oldIrql);
        Core_EventPulse(&page_writer_done, false);
    }
}
page_list Mm_DirtyPageList;
page_list Mm_StandbyPageList;
size_t Mm_DirtyPagesBytes;
size_t Mm_DirtyPagesBytesThreshold = OBOS_PAGE_SIZE * 50;
void Mm_MarkAsDirty(page* pg)
{
    OBOS_ASSERT(!pg->onDirtyList);
    if (pg->onDirtyList)
        return;
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    pg->ln_node.data = pg;
    pg->onStandbyList = false;
    pg->onDirtyList = true;
    APPEND_PAGE_NODE(Mm_DirtyPageList, &pg->ln_node);
    Mm_DirtyPagesBytes += pg->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    Core_SpinlockRelease(&swap_lock, oldIrql);
    if (Mm_DirtyPagesBytes > Mm_DirtyPagesBytesThreshold)
        Mm_WakePageWriter(true);
}
static void mark_standby(page* pg)
{
    pg->ln_node.data = pg;
    pg->onDirtyList = false;
    pg->onStandbyList = true;
    Mm_DirtyPagesBytes -= pg->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    REMOVE_PAGE_NODE(Mm_DirtyPageList, &pg->ln_node);
    APPEND_PAGE_NODE(Mm_StandbyPageList, &pg->ln_node);
}
void Mm_MarkAsStandby(page* pg)
{
    irql oldIrql = Core_SpinlockAcquire(&swap_lock);
    mark_standby(pg);
    Core_SpinlockRelease(&swap_lock, oldIrql);
}
void Mm_InitializePageWriter()
{
    thread_ctx ctx;
    CoreS_SetupThreadContext(&ctx, (uintptr_t)page_writer, 0, false,  Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x20000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 0x20000);
    CoreH_ThreadInitialize(&page_writer_thread, THREAD_PRIORITY_URGENT, Core_DefaultThreadAffinity, &ctx);
    CoreH_ThreadReadyNode(&page_writer_thread, &page_writer_thread_node);
    page_writer_wake = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    page_writer_done = EVENT_INITIALIZE(EVENT_NOTIFICATION);
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