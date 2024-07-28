/*
 * oboskrnl/mm/handler.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <mm/handler.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/swap.h>

#include <utils/tree.h>

#include <locks/spinlock.h>

obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec)
{
    OBOS_ASSERT(ctx);
    page what = {.addr=addr};
    page* page = RB_FIND(page_tree, &ctx->pages, &what);
    if (!page)
        return OBOS_STATUS_UNHANDLED;
    bool pagedOut = page->pagedOut;
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
    if (pagedOut && !page->pagedOut)
    {
        // The page was paged in by someone else...
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    } 
    if (page->pagedOut && !(ec & PF_EC_PRESENT))
    {
        // To not waste time, check if the access is even allowed in the first place.
        if (ec & PF_EC_EXEC && !page->prot.executable)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
        }
        if (ec & PF_EC_RW && !page->prot.rw)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
        }
        if (ec & PF_EC_UM && !page->prot.user)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_UNHANDLED; // TODO: Signal the thread.
        }
        // If this assertion fails, something stupid has happened.
        OBOS_ASSERT(!page->inWorkingSet);
        obos_status status = Mm_SwapIn(page);
        if (obos_likely_error(status))
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return status;
        }
        page->ln_node.data = page;
        page->age |= 1;
        page->prot.touched = false;
        APPEND_PAGE_NODE(ctx->referenced, &page->ln_node);
        // TODO: Try to figure out a good number based off the count of pages in the context/working-set.
        const size_t threshold = 10;
        if (ctx->referenced.nNodes >= threshold)
            Mm_RunPRA(ctx);
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    // TODO: Signal the thread.
    return OBOS_STATUS_UNHANDLED;
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

    size_t szWorkingSet = 0;

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
            szWorkingSet += page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
            node = node->next;
            continue;
        }
        // Remove this page from the working set.
        page_node* next = node->next;
        REMOVE_PAGE_NODE(ctx->workingSet.pages, node);
        obos_status status = Mm_SwapOut(page);
        if (obos_likely_error(status))
        {
            static const char format1[] = 
                "Could not swap out page. Status: %d. Ignoring...\n";
            OBOS_Warning(format1, status);
        }
        // Pop the last referenced page and put it into the working-set
        struct page_node* replacement = ctx->referenced.tail;
        szWorkingSet += replacement->data->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        REMOVE_PAGE_NODE(ctx->referenced, ctx->referenced.tail);
        node->next = node->prev = nullptr;
        APPEND_PAGE_NODE(ctx->workingSet.pages, replacement);
        node = next;
    }

    if (szWorkingSet > ctx->workingSet.size)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Pages in working-set exceeded its size. Size of pages: %lu, size of working set: %lu.\n", szWorkingSet, ctx->workingSet.size);
    if (szWorkingSet == ctx->workingSet.size)
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
        if (szWorkingSet < ctx->workingSet.size)
        {
            szWorkingSet += page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
            APPEND_PAGE_NODE(ctx->workingSet.pages, node); // Only add the page if we have space in the working-set.
        }
        else
        {
            // This page needs to be swapped out as well as removed from the referenced list.
            Mm_SwapOut(page);
        }
        node = next;
    }

    OBOS_ASSERT(szWorkingSet <= ctx->workingSet.size);
    OBOS_ASSERT(!ctx->referenced.nNodes);

    done:
    return OBOS_STATUS_SUCCESS;
}