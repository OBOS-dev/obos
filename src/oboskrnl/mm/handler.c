/*
 * oboskrnl/mm/handler.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/handler.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/swap.h>

#include <utils/tree.h>

#include <locks/spinlock.h>

obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec)
{
    page what = {.addr=addr};
    page* page = RB_FIND(page_tree, &ctx->pages, &what);
    if (!page)
        return OBOS_STATUS_UNHANDLED;
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
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
        obos_status status = Mm_SwapIn(page);
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        if (obos_likely_error(status))
            return status;
        return OBOS_STATUS_SUCCESS;
    }
    Core_SpinlockRelease(&ctx->lock, oldIrql);
     // TODO: Signal the thread.
    return OBOS_STATUS_UNHANDLED;
}
obos_status Mm_RunPRE(context* ctx)
{
    return OBOS_STATUS_UNIMPLEMENTED;
}