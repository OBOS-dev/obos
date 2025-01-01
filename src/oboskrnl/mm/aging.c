/*
 * oboskrnl/mm/aging.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#if defined(OBOS_PAGE_REPLACEMENT_AGING)
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
#include <mm/alloc.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <utils/tree.h>

#include <locks/spinlock.h>
#include <locks/mutex.h>

obos_status Mm_AgingPRA(context* ctx)
{
    // NOTE: These comments were previously in handler.c, for anyone curious.

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

    // NOTE(oberrow, 18:41 2024-09-11):
    // I hate this.
    // NOTE(oberrow, 07:40 2024-09-21):
    // I have finally gotten around to rewriting this.
    
    long workingSetDifference = 0;
    for (working_set_node* node = ctx->workingSet.pages.head; node; )
    {
        working_set_node* const node_save = node;
        working_set_entry* const ent = node->data;
        node = node->next;
        if (ent->free)
        {
            MmH_RemovePageFromWorkingset(ctx, node_save);
            Mm_Allocator->Free(Mm_Allocator, ent, sizeof(*ent));
            continue;
        }
        page_info info = {};
        MmS_QueryPageInfo(ctx->pt, ent->info.virt, &info, nullptr);
        if (info.accessed || info.dirty)
            ent->age |= 1;
        ent->age <<= 1;
        if (ent->age)
            continue;
        workingSetDifference -= ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
        MmH_RemovePageFromWorkingset(ctx, node_save);
    }
    ctx->workingSet.size += workingSetDifference;
    if (ctx->workingSet.size == ctx->workingSet.capacity)
        goto done; // We have no more to do.
    for (working_set_node* curr = ctx->referenced.head; curr; )
    {
        working_set_entry* const ent = curr->data;
        working_set_node* const node = curr;
        curr = node->next;
        REMOVE_WORKINGSET_PAGE_NODE(ctx->referenced, node);
        if (ent->free)
        {
            Mm_Allocator->Free(Mm_Allocator, node, sizeof(*node));
            Mm_Allocator->Free(Mm_Allocator, ent, sizeof(*ent));
            continue;
        }
        ent->age <<= 1;
        node->next = nullptr;
        node->prev = nullptr;
        if (ctx->workingSet.size < ctx->workingSet.capacity)
        {
            ent->workingSets++;
            ctx->workingSet.size += ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
            APPEND_PAGE_NODE(ctx->workingSet.pages, node); // Only add the page if we have space in the working-set.
        }
        else 
        {
            if (!ent->info.range->pageable)
                ent->info.range->pageable = true;
            Mm_SwapOut(ent->info.virt, ent->info.range);
            ctx->stat.paged += (ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        }
    }

    OBOS_ASSERT(ctx->workingSet.size <= ctx->workingSet.capacity);
    OBOS_ASSERT(!ctx->referenced.nNodes);
    done:
    
    if (ctx->workingSet.size > ctx->workingSet.capacity)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Pages in working-set exceeded its size. Size of pages: %lu, size of working set: %lu.\n", ctx->workingSet.size, ctx->workingSet.capacity);

    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_AgingReferencePage(context* ctx, working_set_node* node)
{
    OBOS_UNUSED(ctx);
    node->data->age |= 1;
    return OBOS_STATUS_SUCCESS;
}
#else
#include <int.h>
#include <klog.h>
obos_status Mm_AgingPRA(struct context* ctx)
{
    OBOS_ASSERT(!"what the hell?");
}
obos_status Mm_AgingReferencePage(struct context* ctx, struct working_set_node* node)
{
    OBOS_ASSERT(!"what the hell?");
}
#endif
