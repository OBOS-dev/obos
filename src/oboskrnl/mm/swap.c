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
#include <stdint.h>

swap_dev* Mm_SwapProvider;

obos_status Mm_SwapOut(page* page)
{
    if (!Mm_SwapProvider)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!page)
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
    status = OBOSS_FreePhysicalPages(phys, nPages);
    if (obos_is_error(status))
    {
        if (obos_is_error(Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        page->prot.present = true;
        if (obos_is_error(MmS_SetPageMapping(page->owner->pt, page, phys)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
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
    if (!page->pageable || !page->pagedOut)
        return OBOS_STATUS_SUCCESS;
    size_t nPages = page->prot.huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    uintptr_t id = page->swapId;
    // Allocate a physical page.
    obos_status status = OBOS_STATUS_SUCCESS;
    uintptr_t phys = OBOSS_AllocatePhysicalPages(nPages, nPages, &status);
    if (obos_is_error(status))
        return status;
    // Read the page from the swap.
    status = Mm_SwapProvider->swap_read(Mm_SwapProvider, id, phys, nPages, 0);
    if (obos_is_error(status))
    {
        if (obos_is_error(OBOSS_FreePhysicalPages(phys, nPages)))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Free the swap space.
    status = Mm_SwapProvider->swap_free(Mm_SwapProvider, id, nPages);
    if (obos_is_error(status))
    {
        if (obos_is_error(OBOSS_FreePhysicalPages(phys, nPages)))
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