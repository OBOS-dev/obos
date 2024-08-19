/*
 * oboskrnl/mm/handler.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "mm/alloc.h"
#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <mm/handler.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/swap.h>
#include <mm/pmm.h>
#include <mm/bare_map.h>

#include <vfs/pagecache.h>
#include <vfs/vnode.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <utils/tree.h>

#include <locks/spinlock.h>
#include <locks/mutex.h>

static void handle_oom(context* ctx, size_t bytesNeeded, page* pg)
{
    page* chose = nullptr;
    size_t szChose = 0;
    bool lockAcquired = Core_SpinlockAcquired(&ctx->lock);
    for (page_node* node = ctx->referenced.head; node; )
    {
        page* const curr = node->data;
        if (pg == curr)
        {
            node = node->next;
            continue;
        }
        size_t bytesUsed = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        if (bytesUsed >= bytesNeeded && bytesUsed <= szChose)
        {
            chose = curr;
            szChose = bytesUsed;
        }
        node = node->next;
    }
    try_again:
    if (!chose)
    {
        // Block until there is enough memory to satisfy the OOM.
        if (lockAcquired)
            Core_SpinlockForcedRelease(&ctx->lock);
        CoreH_ThreadListAppend(&Mm_ThreadsAwaitingPhysicalMemory, &Core_GetCurrentThread()->phys_mem_node);
        CoreH_ThreadBlock(Core_GetCurrentThread(), true);
        if (lockAcquired)
            Core_SpinlockAcquire(&ctx->lock);
        return;
    }
    // Swap out the page.
    if (obos_is_error(Mm_SwapOut(chose)))
    {
        chose = nullptr;
        goto try_again;
    }
    ctx->stat.paged += (chose->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    // We've freed enough memory.
    // Return.
    return;
}
static obos_status copy_cow_page(context* ctx, page* page, struct page* other)
{
    void* addr = (void*)page->addr;
    if (other->next_copied_page != page && other->prev_copied_page != page)
    {
        // The other page already did the hard work.
        // We just need to update our page mappings.
        uintptr_t phys = 0;
        OBOSS_GetPagePhysicalAddress((void*)addr, &phys);
        other->prot.rw = true;
        MmS_SetPageMapping(ctx->pt, page, phys);
        return OBOS_STATUS_SUCCESS;
    }
    if (other->pagedOut)
    {
        // This is neat.
        // We can just set prot.rw=true then finish.
        other->prot.rw = true;
        return OBOS_STATUS_SUCCESS;
    }
    // The other page gets the page, while we get a new one.
    uintptr_t phys = page->addr;
    OBOSS_GetPagePhysicalAddress((void*)addr, &phys);
    const size_t pgSize = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    obos_status status = OBOS_STATUS_SUCCESS;
    try_again:
    (void)0;
    uintptr_t newPhys = Mm_AllocatePhysicalPages(pgSize/OBOS_PAGE_SIZE, pgSize/OBOS_PAGE_SIZE, &status);
    if (obos_is_error(status) && status != OBOS_STATUS_NOT_ENOUGH_MEMORY)
       return status;
    if (status == OBOS_STATUS_NOT_ENOUGH_MEMORY)
    {
        handle_oom(ctx, pgSize, page);
        goto try_again;
    }
    page->prot.rw = true;
    MmS_SetPageMapping(ctx->pt, page, newPhys);
    memcpy((void*)page->addr, MmS_MapVirtFromPhys(phys), pgSize);
    other->prot.rw = true;
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec)
{
    OBOS_ASSERT(ctx);
    page what = {.addr=addr};
    page* page = RB_FIND(page_tree, &ctx->pages, &what);
    if (!page)
        return OBOS_STATUS_UNHANDLED;
    bool handled = false;
    bool pagedOut = page->pagedOut;
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
    if (pagedOut && !page->pagedOut)
    {
        // The page was paged in by someone else...
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    bool requiresPageIn = false;
    OBOS_UNUSED(requiresPageIn);
    // To not waste time, check if the access is even allowed in the first place.
    if (ec & PF_EC_EXEC && !page->prot.executable)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
    }
    // if (ec & PF_EC_RW && !page->prot.rw && !(page->prev_copied_page || page->next_copied_page))
    if (ec & PF_EC_RW && page->prot.ro)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
    }
    if (ec & PF_EC_UM && !page->prot.user)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
    }
    if (page->pagedOut && !(ec & PF_EC_PRESENT))
    {
        // If this assertion fails, something stupid has happened.
        OBOS_ASSERT(!page->workingSets);
        handled = true;
        try_again1:
        requiresPageIn = true;
        obos_status status = Mm_SwapIn(page);
        if (obos_is_error(status) && status != OBOS_STATUS_NOT_ENOUGH_MEMORY)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return status;
        }
        if (status == OBOS_STATUS_NOT_ENOUGH_MEMORY)
        {
            handle_oom(ctx, page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, page);
            goto try_again1;
        }
        Mm_KernelContext.stat.paged -= (page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        page->ln_node.data = page;
        page->age |= 1;
        page->prot.touched = false;
        APPEND_PAGE_NODE(ctx->referenced, &page->ln_node);
        // TODO: Try to figure out a better number based off the count of pages in the context/working-set.
        const size_t threshold = (ctx->workingSet.capacity / 4) / OBOS_PAGE_SIZE;
        if (ctx->referenced.nNodes >= threshold)
            Mm_RunPRA(ctx);
    }
    if ((page->next_copied_page || page->prev_copied_page) && ec & PF_EC_RW)
    {
        handled = true;
        // CoW.
        for (struct page* cur = page->next_copied_page; cur; )
        {
            copy_cow_page(ctx, cur->prev_copied_page, cur);
            struct page* next = cur->next_copied_page;
            if (cur->prev_copied_page)
                cur->prev_copied_page->next_copied_page = cur->next_copied_page;
            if (cur->next_copied_page)
                cur->next_copied_page->prev_copied_page = cur->prev_copied_page;
            cur = next;
        }
        for (struct page* cur = page->prev_copied_page; cur; )
        {
            copy_cow_page(ctx, cur->next_copied_page, cur);
            struct page* next = cur->prev_copied_page;
            if (cur->prev_copied_page)
                cur->prev_copied_page->next_copied_page = cur->next_copied_page;
            if (cur->next_copied_page)
                cur->next_copied_page->prev_copied_page = cur->prev_copied_page;
            cur = next;
        }
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    if (page->region && !(ec & PF_EC_PRESENT))
    {
        // We need to map part of the file in.
        handled = true;
        // TODO: Get the vnode in a more sane manner.
        vnode* vn = (vnode*)((uintptr_t)page->region->owner - offsetof(vnode, pagecache));
        OBOS_ASSERT(vn->filesize);
        OBOS_ASSERT(vn->filesize > (page->region->fileoff + page->region->sz));
        // const size_t pgSize = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        if (page->region->owner->sz >= page->region->fileoff)
            VfsH_PageCacheResize(page->region->owner, vn, page->region->fileoff + page->region->sz);
        void* pagecache_region = page->region->owner->data + (page->region->fileoff+(addr - page->region->addr));
        Core_MutexAcquire(&page->region->lock);
        what.addr = (uintptr_t)pagecache_region;
        struct page* pc_page = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        if (page->isPrivateMapping)
        {
            // Simply make both this page, and the page cache's page mapped as CoW
            uintptr_t pagePhys = 0;
            if (pc_page->next_copied_page)
                pc_page->next_copied_page->prev_copied_page = page;
            page->next_copied_page = pc_page->next_copied_page;
            page->prev_copied_page = pc_page;
            pc_page->next_copied_page = page;
            page->prot.rw = page->prot.ro;
            pc_page->prot.rw = pc_page->prot.ro;
            page->prot.present = true;
            pc_page->prot.present = true;
            OBOSS_GetPagePhysicalAddress((void*)page->addr, &pagePhys);
            MmS_SetPageMapping(ctx->pt, page, pagePhys);
            OBOSS_GetPagePhysicalAddress(pagecache_region, &pagePhys);
            MmS_SetPageMapping(Mm_KernelContext.pt, pc_page, pagePhys);
        }
        else 
        {
            // We can just set this page's current physical address to that of the page cache.
            uintptr_t pagePhys = 0;
            page->prot.present = true;
            OBOSS_GetPagePhysicalAddress(pagecache_region, &pagePhys);
            MmS_SetPageMapping(ctx->pt, page, pagePhys);
        }
        Core_MutexRelease(&page->region->lock);
    }
    if (page->region && !page->prot.ro && (ec & PF_EC_RW) && (ec && PF_EC_PRESENT))
    {
        pagecache_dirty_region* dirty_reg = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(pagecache_dirty_region), nullptr);
        dirty_reg->fileoff = page->region->fileoff+(addr - page->region->addr);
        dirty_reg->sz = page->region->sz-page->region->fileoff+(addr - page->region->addr);
        dirty_reg->sz -= dirty_reg->sz % OBOS_PAGE_SIZE;
        dirty_reg->owner = page->region->owner;
        // TODO: Get the vnode in a more sane manner.
        vnode* vn = (vnode*)((uintptr_t)page->region->owner - offsetof(vnode, pagecache));
        OBOS_ASSERT(vn->filesize);
        OBOS_ASSERT(vn->filesize > (page->region->fileoff + page->region->sz));
        LIST_APPEND(dirty_pc_list, &vn->pagecache.dirty_regions, dirty_reg);
        uintptr_t pagePhys = 0;
        OBOSS_GetPagePhysicalAddress((void*)page->addr, &pagePhys);
        page->prot.rw = true;
        MmS_SetPageMapping(ctx->pt, page, pagePhys);
    }

    // TODO: Signal the thread if handled == false.
    return !handled ? OBOS_STATUS_UNHANDLED : OBOS_STATUS_SUCCESS;
}
obos_status Mm_RunPRA(context* ctx)
{
    // NOTE(oberrow, 23:47 2024-07-14):
    // Bah.
    // I need to write this.
    // Maybe I should go to sleep though....
    // Nah.

    // Basically how this works is:
    // The page replacement algorithm goes through each page in the working set and the referenced list.
    // For each page in there, it runs the "aging" PR algorithm.
    // This algorithm basically states that node has a string of 8 bits.
    // > Each time a page reference occurs 1 -> u0.
    // > At the end of each sampling interval or, the bit pattern contained in u0,u1, ... ,
    // > uk is shifted one position, a 0 enters u0, and uk is discarded
    // The sampling interval in our case, is the time between two calls of this function.
    // Any page in the working-set with an "age" of 0 is removed, and is replaced with one in the referenced list.
    // The working set shall never exceed it's set size in the context structure.
    // NOTE(oberrow, 00:12 2024-07-15):
    // I'm just going to go to sleep and continue on with this tomorrow.
    // NOTE(oberrow, 07:11 2024-07-15):
    // Now that I'm awake I can start on this.

    int64_t workingSetDifference = 0;
    for (page_node* node = ctx->workingSet.pages.head; node;)
    {
        page* page = node->data;
        OBOS_ASSERT(page);
        if (page->pagedOut)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Page 0x%p is in working set, and is paged out.\n", (void*)page->addr);
        MmS_QueryPageInfo(ctx->pt, page->addr, page);
        if (page->prot.touched)
            page->age |= 1;
        page->age <<= 1;
        if (page->age)
        {
            node = node->next;
            continue;
        }
        // Remove this page from the working set.
        page_node* next = node->next;
        REMOVE_PAGE_NODE(ctx->workingSet.pages, node);
        node->next = node->prev = nullptr;
        if (!(--page->workingSets))
        {
            obos_status status = Mm_SwapOut(page);
            if (obos_is_success(status))
                ctx->stat.paged += (page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        }
        workingSetDifference -= (page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        // Pop the last referenced page and put it into the working-set
        if ((ctx->workingSet.size + workingSetDifference) < ctx->workingSet.capacity && ctx->referenced.nNodes)
        {
            struct page_node* replacement = ctx->referenced.tail;
            REMOVE_PAGE_NODE(ctx->referenced, replacement);
            replacement->next = replacement->prev = nullptr;
            workingSetDifference += replacement->data->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
            APPEND_PAGE_NODE(ctx->workingSet.pages, replacement);
            replacement->data->workingSets++;
        }
        node = next;
    }
    ctx->workingSet.size += workingSetDifference;

    if (ctx->workingSet.size > ctx->workingSet.capacity)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Pages in working-set exceeded its size. Size of pages: %lu, size of working set: %lu.\n", ctx->workingSet.size, ctx->workingSet.capacity);
    if (ctx->workingSet.size == ctx->workingSet.capacity)
        goto done; // We have no more to do.

    // NOTE(oberrow): A better way to do this would not be to clear the pages, and to instead
    // sort (reverse?) the list for the next sampling interval.
    for (page_node* node = ctx->referenced.tail; node;)
    {
        page_node* next = node->prev;
        page* page = node->data;
        page->age <<= 1; // bit 0 is already set
        REMOVE_PAGE_NODE(ctx->referenced, node);
        node->next = node->prev = nullptr;
        if (ctx->workingSet.size < ctx->workingSet.capacity)
        {
            page->workingSets++;
            ctx->workingSet.size += page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
            APPEND_PAGE_NODE(ctx->workingSet.pages, node); // Only add the page if we have space in the working-set.
        }
        else
        {
            // This page needs to be swapped out as well as removed from the referenced list.
            Mm_SwapOut(page);
            ctx->stat.paged += (page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        }
        node = next;
    }

    OBOS_ASSERT(ctx->workingSet.size <= ctx->workingSet.capacity);
    OBOS_ASSERT(!ctx->referenced.nNodes);

    done:
    return OBOS_STATUS_SUCCESS;
}