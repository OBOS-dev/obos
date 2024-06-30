/*
 * oboskrnl/mm/swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "klog.h"
#include <int.h>
#include <error.h>

#include <mm/context.h>
#include <mm/pg_node.h>
#include <mm/swap.h>
#include <mm/bare_map.h>

OBOS_EXCLUDE_FUNC_FROM_MM obos_status Mm_PageOutPage(swap_dev* swap, page_node* page)
{
    if (!swap || !page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!swap->get_page || !swap->put_page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (page->pagedOut)
        return OBOS_STATUS_SUCCESS; // If it's already paged out, don't page it out again.
    obos_status status = OBOS_STATUS_SUCCESS;
    pt_context pt_ctx = MmS_PTContextGetCurrent();
    pt_context_page_info info = { 0,0,0,0,0,0,0 };
    // Query page info.
    status = MmS_PTContextQueryPageInfo(&pt_ctx, page->addr, &info);
    if (obos_likely_error(status))
        return status;
    // Set present to zero.
    page->present = false;
    page->accessed = false;
    page->dirty = false;
    page->uses = 0;
    status = MmS_PTContextMap(&pt_ctx, page->addr, 0, 0, false, page->huge_page);
    if (obos_likely_error(status))
    {
        obos_status status2 = swap->get_page(swap, page, info.phys);
        if (obos_likely_error(status2))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Page out the page with the swap provider.
    status = swap->put_page(swap, page, info.phys);
    if (obos_likely_error(status))
    {
        obos_status status2 = MmS_PTContextMap(&pt_ctx, page->addr, info.phys, info.protection, true, page->huge_page);
        if (obos_likely_error(status2))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Free the physical page.
    status = OBOSS_FreePhysicalPages(info.phys, info.huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1);
    if (obos_likely_error(status))
    {
        obos_status status2 = MmS_PTContextMap(&pt_ctx, page->addr, info.phys, info.protection, true, page->huge_page);
        if (obos_likely_error(status2))
            return OBOS_STATUS_INTERNAL_ERROR;
        page->present = true;
        status2 = swap->get_page(swap, page, info.phys);
        if (obos_likely_error(status2))
            return OBOS_STATUS_INTERNAL_ERROR;
        return status;
    }
    // Set paged out to true.
    page->pagedOut = true;
    return OBOS_STATUS_SUCCESS;
}
OBOS_EXCLUDE_VAR_FROM_MM swap_dev* Mm_SwapProvider;
OBOS_EXCLUDE_FUNC_FROM_MM obos_status Mm_PageInPage(swap_dev* swap, page_node* page)
{
    if (!swap || !page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!swap->get_page || !swap->put_page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->pagedOut)
        return OBOS_STATUS_SUCCESS; // If it's already paged in, don't page it in again.
    obos_status status = OBOS_STATUS_SUCCESS;
    // We first want to remap the page.
    uintptr_t phys = OBOSS_AllocatePhysicalPages(page->huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1, page->huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1, &status);
    if (obos_likely_error(status))
        return status;
    pt_context pt_ctx = MmS_PTContextGetCurrent();
    page->present = true;
    // Then we want to swap in the page.
    status = swap->get_page(swap, page, phys);
    OBOS_ASSERT(obos_unlikely_error(status));
    if (obos_likely_error(status))
    {
#ifdef OBOS_DEBUG
        OBOS_UNREACHABLE;
#endif
        OBOSS_FreePhysicalPages(phys, page->huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1);
        return status;
    }
    status = MmS_PTContextMap(&pt_ctx, page->addr, phys, page->protection, true, page->huge_page);
    OBOS_ASSERT(obos_unlikely_error(status));
    if (obos_likely_error(status))
    {
#ifdef OBOS_DEBUG
        OBOS_UNREACHABLE;
#endif
        obos_status status2 = swap->put_page(swap, page, phys);
        if (obos_likely_error(status2))
            return OBOS_STATUS_INTERNAL_ERROR;
        OBOSS_FreePhysicalPages(phys, page->huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1);
        return status;
    }
    page->pagedOut = false;
    return OBOS_STATUS_SUCCESS;
}