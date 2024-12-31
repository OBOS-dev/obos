/*
 * oboskrnl/mm/fork.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>

#include <vfs/pagecache.h>

#include <scheduler/schedule.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <locks/spinlock.h>

// NOTE: Does not do CoW.
OBOS_NODISCARD static page_range* clone_page_range(context* new_ctx, const page_range* rng)
{
    page_range* new = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
    *new = *rng;
    new->ctx = new_ctx;
    memzero(&new->working_set_nodes, sizeof(rng->working_set_nodes));
    if (new->mapped_here)
    {
        pagecache_mapped_region* mapped_here = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(pagecache_mapped_region), nullptr);
        *mapped_here = *rng->mapped_here;
        mapped_here->ctx = new_ctx;
        new->mapped_here = mapped_here;
    }
    RB_INSERT(page_tree, &new_ctx->pages, new);

    return new;
}
static void remap_page_range(page_range* rng)
{
    for (uintptr_t virt = rng->virt; virt < (rng->size + rng->virt); virt += (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE))
    {
        page_info info = {};
        MmS_QueryPageInfo(rng->ctx->pt, virt, &info, nullptr);
        info.prot.executable = info.prot.executable  && rng->prot.executable;
        info.prot.rw = info.prot.rw && rng->prot.rw;
        info.prot.ro = info.prot.ro && rng->prot.ro;
        info.prot.user = info.prot.user && rng->prot.user;
        info.prot.uc = info.prot.uc && rng->prot.uc;
        MmS_SetPageMapping(rng->ctx->pt, &info, info.phys, false);
    }
}

obos_status Mm_ForkContext(context* into, context* toFork)
{
    if (!toFork || !into)
        return OBOS_STATUS_INVALID_ARGUMENT;

    irql oldIrql = Core_SpinlockAcquire(&toFork->lock);
    page_range* curr = nullptr;
    RB_FOREACH(curr, page_tree, &toFork->pages)
    {
        if (curr->can_fork)
        {
            if (curr->mapped_here && !curr->cow)
            {
                // If shared, then simply copy the range and continue.
                remap_page_range(clone_page_range(into, curr));
                continue;
            }
            if (!curr->cow)
            {
                // Make pages be Symmetric CoW.
                curr->cow = true;
                curr->un.cow_type = COW_SYMMETRIC;
                if (!curr->prot.ro)
                {
                    curr->prot.rw = false;
                    remap_page_range(curr);
                }
            }
            remap_page_range(clone_page_range(into, curr));
        }
    }
    Core_SpinlockRelease(&toFork->lock, oldIrql);

    return OBOS_STATUS_SUCCESS;
}
