/*
 * oboskrnl/mm/swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "locks/spinlock.h"
#include "utils/tree.h"
#include <int.h>
#include <klog.h>
#include <error.h>

#include <mm/bare_map.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/pmm.h>

swap_dev* Mm_SwapProvider;

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
    size_t nPages = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    uintptr_t id;
    uintptr_t phys = 0;
    // TODO: Use a function that takes in a context.
    obos_status status = OBOSS_GetPagePhysicalAddress((void*)page->addr, &phys);
    if (obos_is_error(status))
        return status;
    // Reserve swap space, then write the page to the reserved swap space.
    status = Mm_SwapProvider->swap_resv(Mm_SwapProvider, &id, nPages);
    if (obos_is_error(status))
        return status;
    status = Mm_SwapProvider->swap_write(Mm_SwapProvider, id, phys, nPages, 0);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Page the page out, and free the backing page.
    page->prot.present = false;
    page->swapId = id;
    status = MmS_SetPageMapping(page->owner->pt, page, 0);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    if (phys)
    {
        // FIXME: Why is phys sometimes set to zero?
        status = Mm_FreePhysicalPages(phys, nPages);
        if (obos_is_error(status))
        {
            if (obos_is_error(Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages)))
                return OBOS_STATUS_INTERNAL_ERROR;
            page->prot.present = true;
            if (obos_is_error(MmS_SetPageMapping(page->owner->pt, page, phys)))
                return OBOS_STATUS_INTERNAL_ERROR;
            return status;
        }
    }
    page->pagedOut = true;
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
    if (!page->pageable || !page->pagedOut)
        return OBOS_STATUS_SUCCESS;
    size_t nPages = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    uintptr_t id = page->swapId;
    // Allocate a physical page.
    obos_status status = OBOS_STATUS_SUCCESS;
    uintptr_t phys = Mm_AllocatePhysicalPages(nPages, nPages, &status);
    if (obos_is_error(status))
        return status;
    // Read the page from the swap.
    status = Mm_SwapProvider->swap_read(Mm_SwapProvider, id, phys, nPages, 0);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_FreePhysicalPages(phys, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Free the swap space.
    status = Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_FreePhysicalPages(phys, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Re-map the page.
    page->prot.present = true;
    status = MmS_SetPageMapping(page->owner->pt, page, phys);
    if (obos_is_error(status)) // Give up if this fails.
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not remap page 0x%p. Status: %d.\n", page->addr, status);
    page->pagedOut = false;
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
    // FIXME: Use each context instead of just the kernel context.
    irql oldIrql = Core_SpinlockAcquire(&Mm_KernelContext.lock);
    uintptr_t inter = Mm_AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE, 1, nullptr);
    RB_FOREACH(curr, page_tree, &Mm_KernelContext.pages)
    {
        if (!curr->pagedOut)
            continue;
        Mm_SwapProvider->swap_read(Mm_SwapProvider, curr->swapId, inter, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE, 0);
        Mm_SwapProvider->swap_free(Mm_SwapProvider, curr->swapId, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE);
        uintptr_t id = 0;
        to->swap_resv(to, &id, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE);
        curr->swapId = id;
        to->swap_write(to, curr->swapId, inter, (curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)/OBOS_PAGE_SIZE, 0);
    }
    Mm_SwapProvider = to;
    Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}