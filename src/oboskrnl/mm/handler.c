/*
 * oboskrnl/mm/handler.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "irq/irql.h"
#include "locks/spinlock.h"
#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <mm/context.h>
#include <mm/pg_node.h>
#include <mm/swap.h>
#include <mm/handler.h>
#include <mm/init.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

#include <utils/tree.h>

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

OBOS_EXCLUDE_FUNC_FROM_MM obos_status Mm_AgePagesInContext(context* ctx)
{
    if (!ctx)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // FUN FACT:
    // These three lines, if uncommented, will make the VMM go to shit.
    // "Why?" you might ask? Because if the process struct is paged out, we get stuck in an infinite page fault loop.
    // OBOS_ASSERT(ctx->owner->ctx == ctx);
    // if (ctx->owner->ctx != ctx)
    //     return OBOS_STATUS_INVALID_ARGUMENT; // Wut?
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_MASKED, true);
    page_node* i = nullptr;
    pt_context_page_info info;
    memzero(&info, sizeof(info));
    // Search pages in the working-set, and any that were accessed otherwise (ctx->pagesReferenced).
    i = ctx->workingSet.list.head;
    obos_status status = OBOS_STATUS_SUCCESS;
    size_t workingSetSize = 0;
    // Remove uncommonly used pages...
    while(i)
    {
        page_node *next = i->linked_list_node.next;
        status = MmS_PTContextQueryPageInfo(&ctx->pt_ctx, i->addr, &info);
        if (obos_likely_error(status))
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not query page info on page 0x%p. Status: %d.\n", i->addr, status);
        i->accessed = info.accessed;
        i->dirty = info.dirty;
        if (i->accessed || i->dirty)
        {
            MmH_RegisterUse(i);
            workingSetSize += i->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        }
	    i->uses <<= 1;
        if (!MmH_LogicalSumOfUses(i->uses))
        {
            // DDDDDDDDIIIIIIIIIIIIIIIIIIIIIIIIIIIIIEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE!!!!!!!!!!!!!!!!!!!!!!!
            // Page out the page and remove it from the working set.
            REMOVE_PAGE_NODE(ctx->workingSet.list, i);
            Mm_PageOutPage(Mm_SwapProvider, i);
        }
        i = next;
    }
    // Add newly used pages.
    i = ctx->pagesReferenced.head;
    size_t szPagesAdded = 0, threshold = ctx->workingSet.size - workingSetSize;
    while (i && szPagesAdded < threshold)
    {
        page_node *next = i->linked_list_node.next;
	    i->uses <<= 1;
        REMOVE_PAGE_NODE(ctx->pagesReferenced, i);
        APPEND_PAGE_NODE(ctx->workingSet.list, i);
        szPagesAdded += i->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        i = next;
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXCLUDE_FUNC_FROM_MM obos_status Mm_OnPageFault(context* ctx, uint32_t ec, uintptr_t addr)
{
    if (!ctx)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!Mm_Initialized())
        return OBOS_STATUS_UNHANDLED;
    page_node what = {.addr=addr};
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_MASKED, true);
    page_node* page = RB_FIND(page_node_tree, &ctx->pageNodeTree, &what);
    if (!page)
    {
        // Not true, we could always page fault on pages in the non-paged pool.
        // OBOS_ASSERT(!(ec & PF_EC_PRESENT)); // If this fails, something bad happened.
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_UNHANDLED;
    }
    irql oldIrql2 = Core_SpinlockAcquireExplicit(&page->lock, IRQL_MASKED, true);
    if (addr < OBOS_PAGE_SIZE)
        OBOS_Warning("The zero-page has been allocated.\n");
    if (!(ec & PF_EC_PRESENT))
    {
        if (!page->pagedOut)
        {
            Core_SpinlockRelease(&page->lock, oldIrql2);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_SUCCESS; // Bah.
        }
        const bool inWorkingSet = (page->linked_list_node.next || page->linked_list_node.prev || page == ctx->workingSet.list.head || page == ctx->workingSet.list.tail);
        if (inWorkingSet)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Page in working-set is swapped out.\n");
        obos_status status = Mm_PageInPage(Mm_SwapProvider, page);    
        APPEND_PAGE_NODE(ctx->pagesReferenced, page);
        MmH_RegisterUse(page);
        Core_SpinlockRelease(&page->lock, oldIrql2);
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        // TODO: Try to figure out a good number based off the count of pages in the context
        const size_t threshold = 10;
        if (ctx->pagesReferenced.nNodes >= threshold)
            Mm_AgePagesInContext(ctx);
        return status;
    }
    Core_SpinlockRelease(&page->lock, oldIrql2);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_UNHANDLED;
}