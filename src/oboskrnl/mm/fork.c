/*
 * oboskrnl/mm/fork.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>

#include <vfs/pagecache.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <locks/spinlock.h>

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
            /*
             * We need to:
             * If the pages are already CoW, but are asym CoW, map them into the new context "as is".
             * If the pages are sym cow, TODO.
             * If a page is paged out at time of call, page it in if !pagedCount
             * Map both page ranges as Sym CoW
             * Go to the next range
             */
            page_range* clone = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(clone, curr, sizeof(*curr));
            clone->ctx = into;
            memzero(&clone->rb_node, sizeof(clone->rb_node));
            RB_INSERT(page_tree, &into->pages, clone);

            for (uintptr_t addr = curr->virt; addr < (curr->virt + curr->size); addr += (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE))
            {
                page_info info = {};
                MmS_QueryPageInfo(toFork->pt, addr, &info, nullptr);
                page what = {.phys=info.phys};
                page* phys = (info.prot.is_swap_phys) ? nullptr : RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
                if (phys)
                {
                    if (phys->cow_type == COW_SYMMETRIC)
                    {
                        uintptr_t fault_addr = addr;
                        Core_SpinlockRelease(&toFork->lock, oldIrql);
                        Mm_HandlePageFault(toFork, fault_addr, PF_EC_RW|((uint32_t)info.prot.present<<PF_EC_PRESENT)|PF_EC_UM);
                        oldIrql = Core_SpinlockAcquire(&toFork->lock);
                        MmS_QueryPageInfo(toFork->pt, fault_addr, nullptr, &info.phys);
                        what.phys = info.phys;
                        phys = (info.phys && !info.prot.is_swap_phys) ? RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what) : nullptr;
                    }
                    MmH_RefPage(phys);
                    phys->pagedCount++;
                    if (phys->cow_type != COW_ASYMMETRIC)
                        phys->cow_type = COW_SYMMETRIC;
                    else
                        info.prot.present = false;
                    info.prot.rw = false;
                    MmS_SetPageMapping(toFork->pt, &info, info.phys, false);
                }
                MmS_SetPageMapping(into->pt, &info, info.phys, false);
            }
        }
    }
    Core_SpinlockRelease(&toFork->lock, oldIrql);

    return OBOS_STATUS_SUCCESS;
}
