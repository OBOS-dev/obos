/*
 * oboskrnl/mm/alloc.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/page.h>
#include <mm/bare_map.h>
#include <mm/swap.h>

#include <utils/tree.h>

#include <irq/irql.h>
#include <locks/spinlock.h>

allocator_info* OBOS_NonPagedPoolAllocator;

#define set_statusp(status, to) (status) ? *(status) = (to) : (void)0
void* MmH_FindAvaliableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status)
{
    set_statusp(status, OBOS_STATUS_UNIMPLEMENTED);
    return nullptr;
}
void* Mm_AllocateVirtualMemory(context* ctx, void* base_, size_t size, prot_flags prot, vma_flags flags, obos_status* ustatus)
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
    // We shouldn't reallocate the page(s).
    // Check if they exist so we don't do that by accident.
    page what = {};
    bool exists = false;
    for (uintptr_t addr = base; addr < base + size; addr += pgSize)
    {
        what.addr = addr;
        page* found = RB_FIND(page_tree, &ctx->pages, &what);
        if (found)
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
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_MASKED, true);
    // TODO: Optimize by splitting really big allocations (> OBOS_HUGE_PAGE_SIZE) into huge pages and normal pages.
    size_t nNodes = size / pgSize + (size % pgSize ? 1 : 0);
    page* nodes = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, nNodes, sizeof(page), &status);
    for (size_t i = 0; i < nNodes; i++)
    {
        uintptr_t phys = OBOSS_AllocatePhysicalPages(pgSize/OBOS_PAGE_SIZE, pgSize/OBOS_PAGE_SIZE, &status);
        page* node = &nodes[i];
        node->addr = base + i*pgSize;
        if (flags & VMA_FLAGS_GUARD_PAGE && i == 0)
        {
            node->prot.present = false;
            node->pagedOut = false;
        }
        else
        {
            node->prot.present = true;
            node->prot.huge_page = flags & VMA_FLAGS_HUGE_PAGE;
            node->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
            node->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
            node->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
            node->pagedOut = false;
            node->pageable = !(flags & VMA_FLAGS_NON_PAGED);
            node->owner = ctx;
            node->age = 0;
            node->prot.touched = false;
            status = MmS_SetPageMapping(ctx->pt, node, phys);
            if (obos_likely_error(status))
            {
                // We need to clean up.
                for (size_t j = 0; j < i; j++)
                {
                    nodes[j].prot.present = false;
                    MmS_SetPageMapping(ctx->pt, &nodes[j], 0);
                    RB_REMOVE(page_tree, &ctx->pages, &nodes[j]);
                }
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, nodes, nNodes*sizeof(page));
                set_statusp(ustatus, status);
                return nullptr;
            }
        }
        RB_INSERT(page_tree, &ctx->pages, node);
        memzero((void*)node->addr, pgSize);
    }
    // Page out each page so we don't explode.
    for (size_t i = 0; i < nNodes & !(flags & VMA_FLAGS_NON_PAGED); i++)
        Mm_SwapOut(&nodes[i]);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return (void*)base;
}