/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <mm/context.h>
#include <mm/pg_node.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

obos_status Mm_HandleAgingFault(context* ctx)
{
    if (!ctx)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_ASSERT(ctx->owner->ctx == ctx);
    if (ctx->cowner->ctx != ctx)
        return OBOS_STATUS_INVALID_ARGUMENT; // Wut?
    page_node* i = nullptr;
    RB_FOREACH(i, page_node_tree, &ctx->pageNodeTree)
    {
        uintptr_t addr = i->addr;
        pt_context_page_info info;
        memzero(&info, sizeof(info));
        MmS_PTContextQueryPageInfo(&ctx->pt_ctx, addr, &info);
        // Any updates to the PTE must be registered, and if that doesn't happen, something terrible has happened.
        OBOS_ASSERT(i->huge_page == info.huge_page &&
                    i->accessed == info.accessed &&
                    i->protection == info.protection && 
                    i->present == info.present); 
        i->huge_page = info.huge_page;
        i->accessed = info.accessed;
        i->dirty = info.dirty;
        i->present = info.present;
        i->protection = info.protection;
        if (i->accessed || i->dirty)
        {
            i->accessed = i->dirty = false;
            MmH_RegisterUse(i);
        }
    }
    i = nullptr;
    return OBOS_STATUS_SUCCESS;
}