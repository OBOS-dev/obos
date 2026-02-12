/*
 * oboskrnl/mm/fork.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>

#include <vfs/pagecache.h>

#include <allocators/base.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <locks/spinlock.h>

obos_status Mm_ForkContext(context* into, context* toFork)
{
    if (!toFork || !into)
        return OBOS_STATUS_INVALID_ARGUMENT;

    // memcpy(&into->stat, &toFork->stat, sizeof(memstat));
    into->stat.committedMemory = toFork->stat.committedMemory;
    into->stat.pageable = toFork->stat.pageable;
    into->stat.nonPaged = toFork->stat.nonPaged;
    into->stat.paged = toFork->stat.paged;
    Mm_GlobalMemoryUsage.committedMemory += into->stat.committedMemory;
    Mm_GlobalMemoryUsage.pageable += into->stat.pageable;
    Mm_GlobalMemoryUsage.nonPaged += into->stat.nonPaged;
    Mm_GlobalMemoryUsage.paged += into->stat.paged;

    irql oldIrql = Core_SpinlockAcquire(&toFork->lock);
    page_range* curr = nullptr;
    RB_FOREACH(curr, page_tree, &toFork->pages)
    {
        if (curr->can_fork)
        {
            /*
             * We need to:
             * If the pages are already CoW, but are asym CoW, map them into the new context "as is".
             * If the pages are sym cow, TODO.
             * If a page is paged out at time of call, page it in if !pagedCount
             * Map both page ranges as Sym CoW
             * Go to the next range
             */
            page_range* clone = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(clone, curr, sizeof(*curr));
            memzero(&clone->rb_node, sizeof(clone->rb_node));
            RB_INSERT(page_tree, &into->pages, clone);

            for (uintptr_t addr = curr->virt; addr < (curr->virt + curr->size); addr += (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE))
            {
                page_info info = {};
                MmS_QueryPageInfo(toFork->pt, addr, &info, nullptr);
                page what = {.phys=info.phys};
                Core_MutexAcquire(&Mm_PhysicalPagesLock);
                page* phys = (info.prot.is_swap_phys) ? nullptr : RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
                Core_MutexRelease(&Mm_PhysicalPagesLock);
                if (phys)
                {
                    MmH_RefPage(phys);
                    phys->pagedCount++;
                    if (!phys->backing_vn)
                    {
                        if (phys->cow_type == COW_DISABLED)
                        {
                            phys->cow_type = COW_SYMMETRIC;
                            info.prot.rw = false;
                        }
                        // else
                        //     info.prot.present = false;
                    }
                    
                } else if (info.prot.is_swap_phys)
                {
                    swap_allocation* swap_alloc = MmH_LookupSwapAllocation(info.phys);
                    MmH_RefSwapAllocation(swap_alloc);
                }
                MmS_SetPageMapping(toFork->pt, &info, info.phys, false);
                MmS_SetPageMapping(into->pt, &info, info.phys, false);
            }
            MmS_TLBShootdown(toFork->pt, curr->virt, curr->size);
        }
    }
    Core_SpinlockRelease(&toFork->lock, oldIrql);

    return OBOS_STATUS_SUCCESS;
}
