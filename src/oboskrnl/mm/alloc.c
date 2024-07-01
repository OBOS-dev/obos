/*
 * oboskrnl/mm/alloc.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "irq/irql.h"
#include "locks/spinlock.h"
#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <mm/prot.h>
#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pg_node.h>
#include <mm/bare_map.h>
#include <mm/swap.h>

#include <utils/tree.h>

#define set_statusp(status, to) (status) ? *(status) = (to) : (void)0
OBOS_EXCLUDE_FUNC_FROM_MM void* MmH_FindAvaliableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status)
{
    set_statusp(status, OBOS_STATUS_UNIMPLEMENTED);
    return nullptr;
}
OBOS_EXCLUDE_FUNC_FROM_MM void* Mm_AllocateVirtualMemory(context* ctx, void* base_, size_t size, prot_flags prot, vma_flags flags, obos_status* ustatus)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    set_statusp(ustatus, status);
    if (!ctx || !size)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    uintptr_t base = (uintptr_t)base_;
    const size_t pgSize = (flags & VMA_FLAGS_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    if (base % pgSize)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (size % pgSize)
        size += (pgSize-(size%pgSize));
    if (flags & VMA_FLAGS_GUARD_PAGE)
        size += pgSize;
    top:
    if (!base)
    {
        base = (uintptr_t)MmH_FindAvaliableAddress(ctx, size, flags & ~VMA_FLAGS_GUARD_PAGE, &status);
        if (obos_likely_error(status))
        {
            set_statusp(ustatus, status);
            return nullptr;
        }
    }
    else 
    {
        // We shouldn't reallocate the page(s).
        // Check if they exist so we don't do that by accident.
        page_node what = {};
        bool exists = false;
        for (uintptr_t addr = base; addr < base + size; addr += pgSize)
        {
            what.addr = addr;
            if (RB_FIND(page_node_tree, &ctx->pageNodeTree, &what))
            {
                exists = true;
                break;
            }
        }
        if (exists)
        {
            if (flags & VMA_FLAGS_HINT)
            {
                base = 0;
                goto top;
            }
            else
            {
                set_statusp(ustatus, OBOS_STATUS_IN_USE);
                return nullptr;
            }
        }
    }
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_MASKED, true);
    // TODO: Optimize by splitting really big allocations (> OBOS_HUGE_PAGE_SIZE) into huge pages and normal pages.
    size_t nNodes = size / pgSize + (size % pgSize ? 1 : 0);
    page_node* nodes = Mm_VMMAllocator->ZeroAllocate(Mm_VMMAllocator, nNodes, sizeof(page_node), &status);
    for (size_t i = 0; i < nNodes; i++)
    {
        uintptr_t phys = OBOSS_AllocatePhysicalPages(pgSize/OBOS_PAGE_SIZE, pgSize/OBOS_PAGE_SIZE, &status);
        page_node* node = &nodes[i];
        node->addr = base + i*pgSize;
        if (flags & VMA_FLAGS_GUARD_PAGE && i == 0)
        {
            node->present = false;
            node->pagedOut = false;
        }
        else
        {
            node->present = true;
            node->huge_page = flags & VMA_FLAGS_HUGE_PAGE;
            node->protection = prot;
            node->pagedOut = false;
            node->owner = ctx;
            node->uses = node->dirty = node->accessed = false;
            status = MmS_PTContextMap(&ctx->pt_ctx, node->addr, phys, prot, node->present, node->huge_page);
            if (obos_likely_error(status))
            {
                // We need to clean up.
                for (size_t j = 0; j < i; j++)
                {
                    MmS_PTContextMap(&ctx->pt_ctx, nodes[j].addr, 0, 0, false, nodes[j].huge_page);
                    RB_REMOVE(page_node_tree, &ctx->pageNodeTree, &nodes[j]);
                }
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                Mm_VMMAllocator->Free(Mm_VMMAllocator, nodes, nNodes*sizeof(page_node));
                set_statusp(ustatus, status);
                return nullptr;
            }
        }
        RB_INSERT(page_node_tree, &ctx->pageNodeTree, node);
        memzero((void*)node->addr, pgSize);
    }
    // Page out each page so we don't explode.
    for (size_t i = 0; i < nNodes; i++)
        Mm_PageOutPage(Mm_SwapProvider, &nodes[i]);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return (void*)base;
}