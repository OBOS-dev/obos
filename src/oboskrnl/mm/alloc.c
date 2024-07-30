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

#include <scheduler/process.h>

#include <stdint.h>
#include <utils/tree.h>

#include <irq/irql.h>
#include <locks/spinlock.h>

allocator_info* OBOS_NonPagedPoolAllocator;
allocator_info* Mm_Allocator;

#define set_statusp(status, to) (status) ? *(status) = (to) : (void)0
void* MmH_FindAvaliableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status)
{
    if (!ctx)
    {
        set_statusp(status, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    size_t pgSize = flags & VMA_FLAGS_HUGE_PAGE ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    if (size % pgSize)
        size -= (size % pgSize);
    uintptr_t base = 
        ctx->owner->pid == 1 ?
        OBOS_KERNEL_ADDRESS_SPACE_BASE :
        OBOS_USER_ADDRESS_SPACE_BASE,
              limit = 
        ctx->owner->pid == 1 ?
        OBOS_KERNEL_ADDRESS_SPACE_LIMIT :
        OBOS_USER_ADDRESS_SPACE_LIMIT;
    if (flags & VMA_FLAGS_32BIT)
    {
        base = 0x1000;
        limit = 0xfffff000;
    }
	page* currentNode = nullptr;
	page* lastNode = nullptr;
	uintptr_t lastAddress = base;
	uintptr_t found = 0;
	for (currentNode = RB_MIN(page_tree, &ctx->pages); 
        currentNode; 
        currentNode = RB_NEXT(page_tree, &ctx->pages, currentNode))
    {
		uintptr_t currentNodeAddr = currentNode->addr - (currentNode->addr % pgSize);
		if (currentNodeAddr < base)
		    continue;
        if (currentNodeAddr >= limit)
            break; // Because of the properties of an RB-Tree, we can break here.
		if ((currentNodeAddr - lastAddress) >= (size + pgSize))
		{
			found = lastAddress + (lastNode->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
			break;
		}
		lastAddress = currentNodeAddr + (currentNode->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        lastNode = currentNode;
	}
    if (!found)
	{
		page* currentNode = lastNode;
		if (currentNode)
			found = (currentNode->addr - (currentNode->addr % OBOS_PAGE_SIZE)) + (currentNode->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
		else
			found = base;
	}
	if (!found)
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		return nullptr;
	}
    return (void*)found;
}
void* Mm_VirtualMemoryAlloc(context* ctx, void* base_, size_t size, prot_flags prot, vma_flags flags, obos_status* ustatus)
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
    irql oldIrql = Core_SpinlockAcquireExplicit(&ctx->lock, IRQL_MASKED, true);
    top:
    if (!base)
    {
        base = (uintptr_t)MmH_FindAvaliableAddress(ctx, size, flags & ~VMA_FLAGS_GUARD_PAGE, &status);
        if (obos_likely_error(status))
        {
            set_statusp(ustatus, status);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
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
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return nullptr;
        }
    }
    // TODO: Optimize by splitting really big allocations (> OBOS_HUGE_PAGE_SIZE) into huge pages and normal pages.
    size_t nNodes = size / pgSize + (size % pgSize ? 1 : 0);
    page** nodes = Mm_Allocator->ZeroAllocate(Mm_Allocator, nNodes, sizeof(page*), &status);
    for (size_t i = 0; i < nNodes; i++)
    {
        uintptr_t phys = OBOSS_AllocatePhysicalPages(pgSize/OBOS_PAGE_SIZE, pgSize/OBOS_PAGE_SIZE, &status);
        page* node = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(page), &status);
        nodes[i] = node;
        node->addr = base + i*pgSize;
        node->allocated = true;
        node->owner = ctx;
        node->prot.touched = false;
        node->pagedOut = false;
        node->prot.huge_page = flags & VMA_FLAGS_HUGE_PAGE;
        node->age = 0;
        if (flags & VMA_FLAGS_GUARD_PAGE && i == 0)
        {
            node->prot.present = false;
            node->isGuardPage = true;
            node->pageable = false;
        }
        else
        {
            node->prot.present = true;
            node->prot.huge_page = flags & VMA_FLAGS_HUGE_PAGE;
            node->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
            node->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
            node->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
            node->pageable = !(flags & VMA_FLAGS_NON_PAGED);
            status = MmS_SetPageMapping(ctx->pt, node, phys);
            if (obos_likely_error(status))
            {
                // We need to clean up.
                for (size_t j = 0; j < i; j++)
                {
                    nodes[j]->prot.present = false;
                    MmS_SetPageMapping(ctx->pt, nodes[j], 0);
                    RB_REMOVE(page_tree, &ctx->pages, nodes[j]);
                    Mm_Allocator->Free(Mm_Allocator, nodes[j], sizeof(page));
                }
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                Mm_Allocator->Free(Mm_Allocator, node, sizeof(page));
                Mm_Allocator->Free(Mm_Allocator, nodes, nNodes*sizeof(page*));
                set_statusp(ustatus, status);
                return nullptr;
            }
            memzero((void*)node->addr, pgSize);
        }
        RB_INSERT(page_tree, &ctx->pages, node);
    }
    // Page out each page so we don't explode.
    // TODO: Error handling?
    for (size_t i = 0; i < nNodes && !(flags & VMA_FLAGS_NON_PAGED); i++)
        Mm_SwapOut(nodes[i]);
    Mm_Allocator->Free(Mm_Allocator, nodes, nNodes*sizeof(page*));
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    if (flags & VMA_FLAGS_GUARD_PAGE)
        base += pgSize;
    return (void*)base;
}
obos_status Mm_VirtualMemoryFree(context* ctx, void* base_, size_t size)
{
    uintptr_t base = (uintptr_t)base_;
    if (base % OBOS_PAGE_SIZE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    // We need to:
    // - Possibly change the base if there is a guard page.
    // - Unmap the pages
    // - Remove the pages from any VMM data structures (working set, page tree, referenced list)
    
    // Verify the pages' existence.
    
    page what;
    memzero(&what, sizeof(what));
    what.addr = base;

    uintptr_t offset = 0;
    page* baseNode = RB_FIND(page_tree, &ctx->pages, &what); 
    if (!baseNode)
        return OBOS_STATUS_NOT_FOUND;
    page* curr = nullptr;
    for (uintptr_t addr = base; addr < (base + size); addr += offset)
    {
        what.addr = addr;
        if (addr == base)
            curr = baseNode;
        else
            curr = RB_NEXT(page_tree, &ctx->pages, curr);
        if (!curr || curr->addr != addr)
            return OBOS_STATUS_NOT_FOUND;
        offset = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }
    uintptr_t guardPageBase = base;
    what.addr = guardPageBase;
    page* guardPage = RB_FIND(page_tree, &ctx->pages, &what);
    if (guardPage->prot.huge_page)
        guardPageBase -= OBOS_HUGE_PAGE_SIZE;
    else
        guardPageBase -= OBOS_PAGE_SIZE;

    what.addr = guardPageBase;
    guardPage = RB_FIND(page_tree, &ctx->pages, &what);
    if (guardPage && guardPage->isGuardPage)
    {
        baseNode = guardPage;
        base = guardPageBase;
        size += guardPage->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }

    // We've possibly located a guard page that we need to free with the rest of the buffer.
    // Now we must unmap the pages and dereference them.

    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);

    offset = 0;
    curr = nullptr;
    page* next = nullptr;
    for (uintptr_t addr = base; addr < (base + size); addr += offset)
    {
        what.addr = addr;
        if (addr == base)
            curr = baseNode;
        else
            curr = next;
        next = RB_NEXT(page_tree, &ctx->pages, curr);
        if (!curr || curr->addr != addr)
            return OBOS_STATUS_NOT_FOUND;
        if (curr->prot.present)
        {
            curr->prot.present = false;
            MmS_SetPageMapping(ctx->pt, curr, 0); // Unmap the page.
        }
        RB_REMOVE(page_tree, &ctx->pages, curr);
        if (curr->inWorkingSet)
            REMOVE_PAGE_NODE(ctx->workingSet.pages, &curr->ln_node);
        if (curr->age == 1 /* it's in the referenced list */)
            REMOVE_PAGE_NODE(ctx->referenced, &curr->ln_node);
        if (curr->allocated)
            Mm_Allocator->Free(Mm_Allocator, curr, sizeof(*curr));
        offset = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }

    Core_SpinlockRelease(&ctx->lock, oldIrql);

    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_VirtualMemoryProtect(context* ctx, void* base_, size_t size, prot_flags prot, int isPageable)
{
    uintptr_t base = (uintptr_t)base_;
    if (base % OBOS_PAGE_SIZE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    
    page what;
    memzero(&what, sizeof(what));
    what.addr = base;

    // Verify each pages' existence

    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);

    uintptr_t offset = 0;
    page* baseNode = RB_FIND(page_tree, &ctx->pages, &what); 
    if (!baseNode)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    page* curr = nullptr;
    for (uintptr_t addr = base; addr < (base + size); addr += offset)
    {
        what.addr = addr;
        if (addr == base)
            curr = baseNode;
        else
            curr = RB_NEXT(page_tree, &ctx->pages, curr);
        if (!curr || curr->addr != addr)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_NOT_FOUND;
        }
        offset = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }


    offset = 0;
    curr = nullptr;
    page* next = nullptr;
    for (uintptr_t addr = base; addr < (base + size); addr += offset)
    {
        what.addr = addr;
        if (addr == base)
            curr = baseNode;
        else
            curr = next;
        next = RB_NEXT(page_tree, &ctx->pages, curr);
        if (!curr || curr->addr != addr)
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return OBOS_STATUS_NOT_FOUND;
        }
        curr->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        curr->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
        curr->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        if (curr->pagedOut && !isPageable)
        {
            // Page in curr if:
            // The current page is paged out, and we need to change the pageable property from true to false.
            Mm_SwapIn(curr);
        }
        if (isPageable < 2)
            curr->pageable = isPageable;
        uintptr_t phys = 0;
        OBOSS_GetPagePhysicalAddress((void*)curr->addr, &phys);
        MmS_SetPageMapping(ctx->pt, curr, phys);

        offset = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    }

    Core_SpinlockRelease(&ctx->lock, oldIrql);

    return OBOS_STATUS_SUCCESS;
}