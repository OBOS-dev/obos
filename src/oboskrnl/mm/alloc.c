/*
 * oboskrnl/mm/alloc.c
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
#include <mm/bare_map.h>
#include <mm/swap.h>
#include <mm/pmm.h>

#include <scheduler/process.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/pagecache.h>

#include <irq/irql.h>

#include <asan.h>

#include <locks/spinlock.h>
#include <locks/pushlock.h>

allocator_info* OBOS_NonPagedPoolAllocator;
allocator_info* Mm_Allocator;

#define set_statusp(status, to) (status) ? *(status) = (to) : (void)0
void* MmH_FindAvailableAddress(context* ctx, size_t size, vma_flags flags, obos_status* status)
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
        ctx->owner->pid == 0 ?
        OBOS_KERNEL_ADDRESS_SPACE_BASE :
        OBOS_USER_ADDRESS_SPACE_BASE,
              limit =
        ctx->owner->pid == 0 ?
        OBOS_KERNEL_ADDRESS_SPACE_LIMIT :
        OBOS_USER_ADDRESS_SPACE_LIMIT;
#if OBOS_ARCHITECTURE_BITS != 32
    if (flags & VMA_FLAGS_32BIT)
    {
        base = 0x1000;
        limit = 0xfffff000;
    }
#endif
	page_range* currentNode = nullptr;
    page_range what = {.virt=base};
	page_range* lastNode = RB_FIND(page_tree, &ctx->pages, &what);
	uintptr_t lastAddress = base;
	uintptr_t found = 0;
	for (currentNode = RB_MIN(page_tree, &ctx->pages);
        currentNode;
        currentNode = RB_NEXT(page_tree, &ctx->pages, currentNode))
    {
		uintptr_t currentNodeAddr = currentNode->virt;
		if (currentNodeAddr < base)
		    continue;
        if (currentNodeAddr >= limit)
            break; // Because of the properties of an RB-Tree, we can break here.
		if ((currentNodeAddr - lastAddress) >= (size + pgSize + (((currentNodeAddr - lastAddress)) % pgSize)))
		{
            if (!lastNode)
                continue;
            found = lastAddress + (((currentNodeAddr - lastAddress)) % pgSize);
            break;
		}
		lastAddress = currentNodeAddr + currentNode->size;
        lastNode = currentNode;
	}
    if (!found)
	{
		page_range* currentNode = lastNode;
		if (currentNode)
			found = (currentNode->virt + currentNode->size);
		else
			found = base;
	}
	if (!found)
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		return nullptr;
	}
    // OBOS_Debug("%s %p\n", __func__, found);

    return (void*)found;
}

// checks if the pages from base->base+size exist
// internal use only
// returns false when at least one of the pages doesn't exist, otherwise true if they all exist.
static bool pages_exist(context* ctx, void* base, size_t size, bool respectUserProtection, prot_flags kprot)
{
    OBOS_ASSERT(ctx);
    OBOS_ASSERT(base);
    if (!size)
        return false;

    uintptr_t virt = (uintptr_t)base;
    page_range what = {.virt = virt,.size=size};
    page_range* volatile rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
        return false;
    if (respectUserProtection)
    {
        if (kprot & OBOS_PROTECTION_EXECUTABLE)
        {
            OBOS_Error("Kernel is doing shady things, refusing to map user pages as executable inside the kernel address space. If all is in your favour, this is a bug, otherwise it's malware.");
            return false;
        }
        // If the kernel wants an RW mapping, but the user range is RO, then fail.
        if (!!(kprot & OBOS_PROTECTION_READ_ONLY) < rng->prot.ro)
            return false;
        // continue with our checks.
    }

    size_t volatile rng_size = OBOS_MIN(rng->size - (virt-rng->virt), size);

    if (rng_size >= size)
        return true; // all the pages exist in this region

    return pages_exist(ctx, (void*)(rng->size+rng->virt), size - rng_size, respectUserProtection, kprot);
}

void* Mm_VirtualMemoryAlloc(context* ctx, void* base, size_t size, prot_flags prot, vma_flags flags, fd* file, obos_status* status)
{
    return Mm_VirtualMemoryAllocEx(ctx, base, size, prot, flags, file, file ? file->offset : 0, status);
}

page* Mm_AnonPage = nullptr;
page* Mm_UserAnonPage = nullptr;
void* Mm_VirtualMemoryAllocEx(context* ctx, void* base_, size_t size, prot_flags prot, vma_flags flags, fd* file, size_t offset, obos_status* ustatus)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    set_statusp(ustatus, status);
    if (!ctx || !size)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (flags & VMA_FLAGS_RESERVE)
        file = nullptr;
    if (file && flags & VMA_FLAGS_NON_PAGED)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (file && !file->vn)
    {
        set_statusp(ustatus, OBOS_STATUS_UNINITIALIZED);
        return nullptr;
    }
    if (file)
    {
        if ((file->vn->seals & F_SEAL_WRITE) && (~prot & OBOS_PROTECTION_READ_ONLY) && ctx != &Mm_KernelContext)
        {
            set_statusp(ustatus, OBOS_STATUS_ACCESS_DENIED);
            return nullptr;   
        }
    }
    if (file)
        flags &= ~VMA_FLAGS_HUGE_PAGE; // you see, page caches don't really use huge pages, so we have to force huge pages off.
    uintptr_t base = (uintptr_t)base_;
    const size_t pgSize = (flags & VMA_FLAGS_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    if (base % pgSize)
    {
        set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }
    if (OBOS_HUGE_PAGE_SIZE == OBOS_PAGE_SIZE)
        flags &= ~VMA_FLAGS_HUGE_PAGE;
    if (flags & VMA_FLAGS_32BITPHYS)
        file = nullptr;
    if (file)
    {
        if (file->vn->vtype != VNODE_TYPE_REG && file->vn->vtype != VNODE_TYPE_BLK /* hopefully doesn't break */)
        {
            set_statusp(ustatus, OBOS_STATUS_INVALID_ARGUMENT);
            return nullptr;
        }
        if (file->vn->filesize < size)
            size = file->vn->filesize; // Truncated.
        if ((offset+size > file->vn->filesize))
            size = (offset+size) - file->vn->filesize;
        if (size % pgSize)
            size += (pgSize-(size%pgSize));
        if (!(file->flags & FD_FLAGS_READ))
        {
            // No.
            set_statusp(ustatus, OBOS_STATUS_ACCESS_DENIED);
            return nullptr;
        }
        if (!(file->flags & FD_FLAGS_WRITE) && !(flags & VMA_FLAGS_PRIVATE))
            prot |= OBOS_PROTECTION_READ_ONLY;
        file->vn->nMappedRegions++;
        if (~prot & OBOS_PROTECTION_READ_ONLY)
            file->vn->nWriteableMappedRegions++;
    }
    if (size % pgSize)
        size += (pgSize-(size%pgSize));
    if (flags & VMA_FLAGS_GUARD_PAGE)
        size += pgSize;
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
    top:
    if (!base)
    {
        base = (uintptr_t)MmH_FindAvailableAddress(ctx, size, flags & ~VMA_FLAGS_GUARD_PAGE, &status);
        if (obos_is_error(status))
        {
            set_statusp(ustatus, status);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return nullptr;
        }
        OBOS_ASSERT(!(base % pgSize));
    }
    // We shouldn't reallocate the page(s).
    // Check if they exist so we don't do that by accident.
    // page what = {};
    // bool exists = false;
    // for (uintptr_t addr = base; addr < base + size; addr += pgSize)
    // {
    //     what.addr = addr;
    //     page* found = RB_FIND(page_tree, &ctx->pages, &what);
    //     if (found && !found->reserved)
    //     {
    //         exists = true;
    //         break;
    //     }
    // }
    page_range what = {.virt=base,.size=size};
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (rng && !rng->reserved)
    {
        if (flags & VMA_FLAGS_HINT)
        {
            base = 0;
            goto top;
        }
        else
        {
            if (!base_)
                OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "BUG: MmH_FindAvailableAddress returned an address (%p) already in-use\n", base);
            set_statusp(ustatus, OBOS_STATUS_IN_USE);
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            return nullptr;
        }
    }
    if (rng && rng->reserved)
    {
        // Check if the page(s) were already committed, and if so, fail.
        page_info temp = {};
        for (uintptr_t addr = base; addr < (base+size); addr += (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE))
        {
            MmS_QueryPageInfo(ctx->pt, addr, &temp, nullptr);
            page what = {.phys=temp.phys};
            Core_MutexAcquire(&Mm_PhysicalPagesLock);
            page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            Core_MutexRelease(&Mm_PhysicalPagesLock);
            if (pg)
            {
                set_statusp(ustatus, OBOS_STATUS_IN_USE);
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                return nullptr;
            }
            // Uncommitted
            continue;
        }
    }
    // TODO: Optimize by splitting really big allocations (> OBOS_HUGE_PAGE_SIZE) into huge pages and normal pages.
    off_t currFileOff = file ? offset : 0;

    bool present = false;
    volatile bool isNew = true;

    if (!rng)
    {
        rng = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);

        present = rng->prot.present = !(flags & VMA_FLAGS_RESERVE);
        rng->prot.huge_page = flags & VMA_FLAGS_HUGE_PAGE;
        if (!(flags & VMA_FLAGS_PRIVATE) || !file)
        {
            rng->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
            rng->pageable = !(flags & VMA_FLAGS_NON_PAGED);
        }

        rng->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        rng->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        rng->prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        rng->prot.fb = flags & VMA_FLAGS_FRAMEBUFFER;
        if (!rng->prot.fb)
            rng->prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
        rng->hasGuardPage = (flags & VMA_FLAGS_GUARD_PAGE);
        rng->size = size;
        rng->virt = base;

        rng->pageable = ~flags & VMA_FLAGS_NON_PAGED;
        rng->reserved = (flags & VMA_FLAGS_RESERVE);
        rng->can_fork = ~flags & VMA_FLAGS_NO_FORK;

        // this can be implied by doing '!rng->cow && rng->mapped_here'
        // rng->un.shared = reg ? ~flags & VMA_FLAGS_PRIVATE : false;
        rng->phys32 = (flags & VMA_FLAGS_32BITPHYS);
        rng->ctx = ctx;

        if (file)
            rng->un.mapped_vn = file->vn;
    }
    else
    {
        isNew = false;
        // OBOS_UNUSED(isNew);
        rng->size_committed += size;
        if (rng->size_committed >= rng->size)
            rng->reserved = false;
        present = true;
    }

    page* phys = nullptr;
    if (!file && !(flags & VMA_FLAGS_NON_PAGED) && !(flags & VMA_FLAGS_RESERVE))
    {
        OBOS_ASSERT(Mm_AnonPage);
        // Use the anon physical page.
        phys = prot & OBOS_PROTECTION_USER_PAGE ? Mm_UserAnonPage : Mm_AnonPage;
    }
    for (uintptr_t addr = base; addr < (base+size); addr += pgSize)
    {
        bool isPresent = !(rng->hasGuardPage && (base==addr)) && present;
        bool cow = false;
        // for (volatile bool b = (addr==0xffffff00003d3000); b; )
        //     ;

        if (isPresent)
        {
            if (!file && (flags & VMA_FLAGS_NON_PAGED))
            {
                phys = MmH_PgAllocatePhysical(rng->phys32, rng->prot.huge_page);
                if (!phys)
                {
                    RB_REMOVE(page_tree, &ctx->pages, rng);
                    for (uintptr_t jaddr = base; jaddr < addr; jaddr += OBOS_PAGE_SIZE)
                    {
                        page_info info = {};
                        MmS_QueryPageInfo(ctx->pt, jaddr, &info, nullptr);
                        page key = {.phys=info.phys};
                        Core_MutexAcquire(&Mm_PhysicalPagesLock);
                        page* curr_pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &key);
                        Core_MutexRelease(&Mm_PhysicalPagesLock);
                        curr_pg->pagedCount--;
                        MmH_DerefPage(curr_pg);
                        info.prot.present = false;
                        MmS_SetPageMapping(ctx->pt, &info, 0, true);
                    }
                    Free(Mm_Allocator, rng, sizeof(*rng));
                    Core_SpinlockRelease(&ctx->lock, oldIrql);
                    return nullptr;
                }
            }
            else if (file)
            {
                // File page.
                page what = {.backing_vn=file->vn,.file_offset=currFileOff};
                phys = RB_FIND(pagecache_tree, &file->vn->cache, &what);
                if (flags & VMA_FLAGS_PREFAULT && !phys)
                    phys = VfsH_PageCacheCreateEntry(file->vn, currFileOff);
                if (phys)
                {
                    MmH_RefPage(phys);
                    if (cow)
                        phys->cow_type = COW_SYMMETRIC;
                }
            }
            else
            {
                MmH_RefPage(phys);
                phys->cow_type = COW_ASYMMETRIC;
            }
        }

        // Append the virtual page to *phys on demand as apposed to now to save memory.
        // An example of where it'd be added is on swap out, as the virtual_pages list is not needed until then.

        if (phys)
            phys->pagedCount++;

        page_info curr = {};
        curr.range = rng;
        curr.virt = addr;
        curr.phys = phys ? phys->phys : 0;
        curr.prot = rng->prot;
        curr.prot.rw = cow ? false : rng->prot.rw;
        curr.prot.present = isPresent;
        if (phys && phys->cow_type == COW_ASYMMETRIC)
            curr.prot.present = false;
        if (!phys && file)
            curr.prot.present = false;

        MmS_SetPageMapping(ctx->pt, &curr, curr.phys, false);

        currFileOff += pgSize;
    }
    if (!(flags & VMA_FLAGS_RESERVE))
    {
        if (flags & VMA_FLAGS_GUARD_PAGE)
            size -= pgSize;
        if (!(flags & VMA_FLAGS_NON_PAGED))
        {
            ctx->stat.pageable += size;
            Mm_GlobalMemoryUsage.pageable += size;
        }
        else
        {
            ctx->stat.nonPaged += size;
            Mm_GlobalMemoryUsage.nonPaged += size;
        }
        if (!isNew)
        {
            ctx->stat.reserved -= size;
            Mm_GlobalMemoryUsage.reserved -= size;
        }
        else
        {
            ctx->stat.committedMemory += size;
            Mm_GlobalMemoryUsage.committedMemory += size;
        }
    }
    else
    {
        ctx->stat.reserved += size;
        Mm_GlobalMemoryUsage.reserved += size;
    }
    OBOS_ASSERT(rng->size);
    RB_INSERT(page_tree, &ctx->pages, rng);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    if (flags & VMA_FLAGS_GUARD_PAGE)
        base += pgSize;
    // printf("mapped %d bytes at %p\n", size, base);
    return (void*)base;
}

// TODO: Make this support freeing multiple 'page_range's at the same time without bugging out.
obos_status Mm_VirtualMemoryFree(context* ctx, void* base_, size_t size)
{
    // OBOS_Debug("%s %p\n", __func__, base_);
    uintptr_t base = (uintptr_t)base_;
    // if (base % OBOS_PAGE_SIZE)
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    base -= (base%OBOS_PAGE_SIZE);
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    /* We need to:
        - Unmap the pages
        - Remove the pages from any VMM data structures (working set, page tree, referenced list)
    */

    // Verify the pages' existence.
    page_range what = {.virt=base,.size=size};
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    if (rng->size > size)
        size = rng->size; // TODO: Fix

    // printf("freeing %d at %p. called from %p\n", size, base, __builtin_return_address(0));
    bool sizeHasGuardPage = false;
    if (rng->hasGuardPage)
    {
        const size_t pgSize = (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        base -= pgSize;
        if (size == (rng->size - pgSize))
        {
            size += pgSize;
            sizeHasGuardPage = true;
        }
    }

    bool full = true;
    page_protection new_prot = rng->prot;
    new_prot.present = false;
    if (rng->virt != base || rng->size != size)
    {
        // OBOS_Debug("untested code path\n");
        full = false;
        // Split.
        // If rng->virt == base, or rng->size == size
        // Split at rng->virt+size (size: rng->size-size) and remove the other range
        // If rng->size != size, and rng->virt != base
        // Split at base (size: ((base-rng->virt)+rng->size-size)-rng->virt)
        if (rng->virt != base && rng->size != size)
        {
            if ((base + size) >= (rng->virt+rng->size))
            {
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                return OBOS_STATUS_INVALID_ARGUMENT;
            }
            // We need two ranges, one for the range behind base, and another for the range after.
            page_range* before = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* after = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(before, rng, sizeof(*before));
            memcpy(after, rng, sizeof(*before));
            before->size = base-before->virt;
            after->virt = before->virt+before->size+size;
            after->size = (after->virt-before->virt);
            after->hasGuardPage = false;
            memzero(&before->working_set_nodes, sizeof(before->working_set_nodes));
            memzero(&after->working_set_nodes, sizeof(after->working_set_nodes));
            // printf("split %p-%p into %p-%p and %p-%p\n", base, size+base, before->virt, before->virt+before->size, after->virt, after->virt+after->size);
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= before->virt && curr->data->info.virt < after->virt)
                {
                    curr->data->free = true; // mark for deletion.
                    Free(Mm_Allocator, curr, sizeof(*curr));
                    curr = next;
                    continue;
                }
                if (curr->data->info.virt < before->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = before;
                    APPEND_WORKINGSET_PAGE_NODE(before->working_set_nodes, &curr->data->pr_node);
                }
                if (curr->data->info.virt >= after->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = after;
                    APPEND_WORKINGSET_PAGE_NODE(after->working_set_nodes, &curr->data->pr_node);
                }
                curr = next;
            }
            RB_REMOVE(page_tree, &ctx->pages, rng);
            RB_INSERT(page_tree, &ctx->pages, before);
            RB_INSERT(page_tree, &ctx->pages, after);
            rng->ctx = nullptr;
            Free(Mm_Allocator, rng, sizeof(*rng));
            rng = nullptr;
        }
        else if (rng->virt == base || rng->size == size)
        {
            rng->size = (rng->size-size);
            rng->virt += size;
            // printf("split %p-%p into %p-%p\n", base, size+base, rng->virt, rng->virt+rng->size);
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt < rng->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    curr->data->free = true;
                    Free(Mm_Allocator, curr, sizeof(*curr));
                }
                curr = next;
            }
            rng = nullptr;
        }
    }

    // OBOS_ASSERT(rng->size);

    page_info pg = {};
    pg.prot = new_prot;
    pg.range = nullptr;

    for (uintptr_t addr = base; addr < (base+size); addr += new_prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)
    {
        pg.virt = addr;
        page_info info = {};
        MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        info.range = rng;
        if (!info.prot.is_swap_phys && info.phys)
        {
            page what = {.phys=info.phys};
            Core_MutexAcquire(&Mm_PhysicalPagesLock);
            page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
            Core_MutexRelease(&Mm_PhysicalPagesLock);
            if (pg)
            {
                pg->pagedCount--;
                MmH_DerefPage(pg);
            }
        }
        MmS_SetPageMapping(ctx->pt, &pg, 0, true);
    }
    MmS_TLBShootdown(ctx->pt, base, size);

    if (rng)
    {
        if (sizeHasGuardPage)
            size -= (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        if (rng->reserved)
        {
            ctx->stat.reserved -= size;
            Mm_GlobalMemoryUsage.reserved -= size;
        }
        else
        {
            ctx->stat.committedMemory -= size;
            Mm_GlobalMemoryUsage.committedMemory -= size;
        }
        if (rng->pageable)
        {
            ctx->stat.pageable -= size;
            Mm_GlobalMemoryUsage.pageable -= size;
        }
        else
        {
            ctx->stat.nonPaged -= size;
            Mm_GlobalMemoryUsage.nonPaged -= size;
        }
        OBOS_ASSERT(Mm_GlobalMemoryUsage.committedMemory >= 0);
        OBOS_ASSERT(Mm_GlobalMemoryUsage.nonPaged >= 0);
        OBOS_ASSERT(Mm_GlobalMemoryUsage.pageable >= 0);
        OBOS_ASSERT(Mm_GlobalMemoryUsage.reserved >= 0);
    }

    if (full)
    {
        // for (working_set_node* curr = rng->working_set_nodes.head; curr; )
        // {
        //     working_set_node* next = curr->next;
        //     REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
        //     curr->data->free = true;
        //     Free(Mm_Allocator, curr, sizeof(*curr));
        //     curr = next;
        // }
        RB_REMOVE(page_tree, &ctx->pages, rng);
        Free(Mm_Allocator, rng, sizeof(*rng));
    }

    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}

// TODO: Make this support protecting multiple 'page_range's at the same time without bugging out.
obos_status Mm_VirtualMemoryProtect(context* ctx, void* base_, size_t size, prot_flags prot, int isPageable)
{
    uintptr_t base = (uintptr_t)base_;
    if (base % OBOS_PAGE_SIZE)
    {
        size += (base % OBOS_PAGE_SIZE);
        base -= (base%OBOS_PAGE_SIZE);
    }
    if (!ctx || !base || !size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    if (prot == OBOS_PROTECTION_SAME_AS_BEFORE && isPageable > 1)
        return OBOS_STATUS_SUCCESS;
    // Verify the pages' existence.
    page_range what = {.virt=base,.size=size};
    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    page_protection new_prot = rng->prot;
    if (~prot & OBOS_PROTECTION_SAME_AS_BEFORE)
    {
        new_prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        new_prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        new_prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        new_prot.rw = !new_prot.ro;
        new_prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
    }
    else
    {
        if (prot & OBOS_PROTECTION_EXECUTABLE)
            new_prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
        if (prot & OBOS_PROTECTION_USER_PAGE)
            new_prot.user = prot & OBOS_PROTECTION_USER_PAGE;
        if (prot & OBOS_PROTECTION_READ_ONLY)
            new_prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
        if (prot & OBOS_PROTECTION_CACHE_DISABLE)
            new_prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
        if (prot & OBOS_PROTECTION_CACHE_ENABLE)
            new_prot.uc = !(prot & OBOS_PROTECTION_CACHE_ENABLE);
    }
    bool pageable = isPageable > 1 ? rng->pageable : (bool)isPageable;
    if (rng->virt != base || rng->size != size)
    {
        // Split.
        // If rng->virt == base, or rng->size == size
        // Split at rng->virt+size (size: rng->size-size) and remove the other range
        // If rng->size != size, and rng->virt != base
        // Split at base (size: ((base-rng->virt)+rng->size-size)-rng->virt)
        if (rng->virt != base && rng->size != size)
        {
            // OBOS_Debug("untested code path 1\n");
            if ((base + size) > (rng->virt+rng->size))
            {
                // TODO: Support modifying multiple regions at once.
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                return OBOS_STATUS_INVALID_ARGUMENT;
            }
            // We need three ranges, one for the range behind base, another for the range after, and another for the new protection flags.
            page_range* before = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* after = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            page_range* new = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(new, rng, sizeof(*rng));
            memcpy(before, rng, sizeof(*rng));
            memcpy(after, rng, sizeof(*rng));
            before->size = base-before->virt;
            after->virt = before->virt+before->size+size;
            after->size = rng->size-(after->virt-rng->virt);
            memzero(&before->working_set_nodes, sizeof(before->working_set_nodes));
            memzero(&after->working_set_nodes, sizeof(after->working_set_nodes));
            new->prot = new_prot;
            new->pageable = pageable;
            for (working_set_node* curr = rng->working_set_nodes.head; curr; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= before->virt && curr->data->info.virt < after->virt)
                {
                    curr->data->info.range = new;
                    if (!pageable)
                        curr->data->free = true;
                    curr = next;
                    continue;
                }
                if (curr->data->info.virt < before->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    APPEND_WORKINGSET_PAGE_NODE(before->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = before;
                }
                if (curr->data->info.virt >= after->virt)
                {
                    REMOVE_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &curr->data->pr_node);
                    APPEND_WORKINGSET_PAGE_NODE(after->working_set_nodes, &curr->data->pr_node);
                    curr->data->info.range = after;
                }
                curr = next;
            }
            new->prot = new_prot;
            new->pageable = pageable;
            new->virt = base;
            new->size = size;
            // printf(
            //     ""
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n",
            //     before->virt, before->virt+before->size,
            //     new->virt, new->virt+new->size,
            //     after->virt, after->virt+after->size,
            //     rng->virt, rng->virt+rng->size
            // );
            RB_REMOVE(page_tree, &ctx->pages, rng);
            if (before->size)
                RB_INSERT(page_tree, &ctx->pages, before);
            else
                Free(Mm_Allocator, before, sizeof(page_range));
            if (after->size)
                RB_INSERT(page_tree, &ctx->pages, after);
            else
                Free(Mm_Allocator, after, sizeof(page_range));
            if (new->size)
                RB_INSERT(page_tree, &ctx->pages, new);
            else
                Free(Mm_Allocator, before, sizeof(page_range));
            Free(Mm_Allocator, rng, sizeof(*rng));
            rng = new;
        }
        else if (rng->virt == base || rng->size == size)
        {
            // OBOS_Debug("untested code path 2\n");
            // printf(
            //     ""
            //     "0x%p-0x%p\n",
            //     rng->virt, rng->virt+rng->size
            // );
            page_range* new = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
            memcpy(new, rng, sizeof(*rng));
            new->size = size;
            rng->size = (rng->size-size);
            if (base > rng->virt)
                rng->virt = base;
            else
                rng->virt += size;
            // printf(
            //     ""
            //     "0x%p-0x%p\n"
            //     "0x%p-0x%p\n",
            //     rng->virt, rng->virt+rng->size,
            //     new->virt, new->virt+new->size
            // );
            new->prot = new_prot;
            new->pageable = pageable;
            RB_INSERT(page_tree, &ctx->pages, new);
            size_t szUpdated = 0;
            for (working_set_node* curr = rng->working_set_nodes.head; curr && szUpdated < size; )
            {
                working_set_node* next = curr->next;
                if (curr->data->info.virt >= new->virt && curr->data->info.virt < rng->virt)
                {
                    szUpdated += (curr->data->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
                    curr->data->info.range = new;
                    if (!pageable)
                        curr->data->free = true;
                }
                curr = next;
            }
            if (!rng->size)
                RB_REMOVE(page_tree, &ctx->pages, rng);
            rng = new;
        }
    }
    if (rng)
        OBOS_ASSERT(rng->size);
    page_info pg = {};
    pg.prot = new_prot;
    pg.range = nullptr;

    for (uintptr_t addr = base; addr < (base+size); addr += new_prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)
    {
        pg.virt = addr;
        // printf("0x%p %08x\n", pg.virt, prot);
        page_info info = {};
        MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        pg.prot.present = info.prot.present;
        MmS_SetPageMapping(ctx->pt, &pg, info.phys, false);
    }
    MmS_TLBShootdown(ctx->pt, base, size);
    Core_SpinlockRelease(&ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}

void* Mm_MapViewOfUserMemory(context* const user_context, void* ubase_, void* kbase_, size_t size, prot_flags prot, bool respectUserProtection, obos_status *status)
{
    uintptr_t ubase = (uintptr_t)ubase_;
    uintptr_t kbase = (uintptr_t)kbase_;
    size += (ubase%OBOS_PAGE_SIZE);
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    kbase -= (kbase % OBOS_PAGE_SIZE);
    ubase -= (ubase % OBOS_PAGE_SIZE);
    if (!ubase)
    {
        set_statusp(status, OBOS_STATUS_INVALID_ARGUMENT);
        return nullptr;
    }

    irql oldIrql = Core_SpinlockAcquire(&Mm_KernelContext.lock);

    irql oldIrql2 = user_context == &Mm_KernelContext ? IRQL_INVALID : Core_SpinlockAcquire(&user_context->lock);
    if (!pages_exist(user_context, (void*)ubase, size, respectUserProtection, prot))
    {
        Core_SpinlockRelease(&user_context->lock, oldIrql2);
        Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
        set_statusp(status, OBOS_STATUS_PAGE_FAULT);
        return nullptr;
    }

    // This function is undesirable for cheking if memory to be allocated doesn't already exist
    // So just assume the kernel is unfalliable
    // TODO: Fix
    // if (pages_exist(&Mm_KernelContext, (void*)kbase, size))
    // {
    //     Core_SpinlockRelease(&user_context->lock, oldIrql2);
    //     Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
    //     return OBOS_STATUS_INVALID_ARGUMENT;
    // }

    if (!kbase)
    {
        kbase = (uintptr_t)MmH_FindAvailableAddress(&Mm_KernelContext, size, 0, status);
        if (!kbase)
        {
            Core_SpinlockRelease(&user_context->lock, oldIrql2);
            Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
            return nullptr;
        }
    }

    page_range* rng = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
    rng->virt = kbase;
    rng->ctx = &Mm_KernelContext;
    rng->phys32 = false;
    rng->hasGuardPage = false;
    rng->pageable = false;
    rng->size = size;
    rng->prot.huge_page = false;
    rng->prot.rw = !(prot & OBOS_PROTECTION_READ_ONLY);
    rng->pageable = false;
    rng->prot.executable = prot & OBOS_PROTECTION_EXECUTABLE;
    rng->prot.user = prot & OBOS_PROTECTION_USER_PAGE;
    rng->prot.ro = prot & OBOS_PROTECTION_READ_ONLY;
    rng->prot.uc = prot & OBOS_PROTECTION_CACHE_DISABLE;
    RB_INSERT(page_tree, &Mm_KernelContext.pages, rng);

    page_range* user_rng = nullptr;

    for (uintptr_t kaddr = kbase, uaddr = ubase; kaddr < (kbase+size); kaddr += OBOS_PAGE_SIZE, uaddr += OBOS_PAGE_SIZE)
    {
        if (!user_rng || (user_rng->virt+user_rng->size) <= kaddr)
        {
            page_range key = {.virt=uaddr};
            user_rng = RB_FIND(page_tree, &user_context->pages, &key);
            // uh oh
            OBOS_ASSERT(user_rng);
        }

        page* phys = nullptr;
        page_info info = {};
        MmS_QueryPageInfo(user_context->pt, uaddr, &info, nullptr);

        page what = {.phys=info.phys};
        Core_MutexAcquire(&Mm_PhysicalPagesLock);
        phys = (info.phys && !info.prot.is_swap_phys) ? RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what) : nullptr;
        Core_MutexRelease(&Mm_PhysicalPagesLock);
        if (user_rng->un.mapped_vn && !phys)
        {
            Core_SpinlockRelease(&user_context->lock, oldIrql2);
            Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
            VfsH_PageCacheGetEntry(user_rng->un.mapped_vn, uaddr-(user_rng->virt), &phys);
            oldIrql = Core_SpinlockAcquire(&Mm_KernelContext.lock);
            oldIrql2 = Core_SpinlockAcquire(&user_context->lock);
            what.phys = phys->phys;
            info.phys = phys->phys;
            if (~prot & OBOS_PROTECTION_READ_ONLY)
                Mm_MarkAsDirtyPhys(phys);
        }
        if (!info.prot.is_swap_phys)
            OBOS_ENSURE(phys);

        if ((phys && phys->cow_type && ~prot & OBOS_PROTECTION_READ_ONLY) || info.prot.is_swap_phys)
        {
            uintptr_t fault_addr = uaddr;
            fault_addr -= (fault_addr % (user_rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE));
            Core_SpinlockRelease(&user_context->lock, oldIrql2);
            Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
            Mm_HandlePageFault(user_context, fault_addr, PF_EC_RW|((uint32_t)info.prot.present<<PF_EC_PRESENT)|PF_EC_UM);
            oldIrql = Core_SpinlockAcquire(&Mm_KernelContext.lock);
            oldIrql2 = Core_SpinlockAcquire(&user_context->lock);
            MmS_QueryPageInfo(user_context->pt, uaddr, nullptr, &info.phys);
            what.phys = info.phys;
            Core_MutexAcquire(&Mm_PhysicalPagesLock);
            phys = (info.phys && !info.prot.is_swap_phys) ? RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what) : nullptr;
            Core_MutexRelease(&Mm_PhysicalPagesLock);
            OBOS_ENSURE(phys != Mm_AnonPage);
        }

        if (phys)
        {
            MmH_RefPage(phys);
            phys->pagedCount++;
        }

        info.virt = kaddr;
        info.dirty = false;
        info.accessed = false;
        info.range = rng;
        info.prot = rng->prot;
        info.prot.present = info.phys != 0;
        info.prot.rw = rng->prot.rw;
        OBOS_ENSURE(info.phys);
        MmS_SetPageMapping(Mm_KernelContext.pt, &info, info.phys, false);
    }

    if (user_context != &Mm_KernelContext)
        Core_SpinlockRelease(&user_context->lock, oldIrql2);
    Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);

    Mm_KernelContext.stat.committedMemory += size;
    Mm_KernelContext.stat.nonPaged += size;
    Mm_GlobalMemoryUsage.committedMemory += size;
    Mm_GlobalMemoryUsage.nonPaged += size;

    set_statusp(status, OBOS_STATUS_SUCCESS);
    return (void*)(kbase + ((uintptr_t)ubase_ % OBOS_PAGE_SIZE));
}

void* Mm_AllocateKernelStack(context* target_user, obos_status* status)
{
    const size_t sz = 0x10000;
    void* base = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, sz, 0, VMA_FLAGS_KERNEL_STACK, nullptr, status);
    if (!base)
        return nullptr;
    page_range what = {.virt=(uintptr_t)base,.size=sz};
    page_range* rng = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
    rng->kernelStack = true;
    rng->un.userContext = target_user;

    for (uintptr_t addr = rng->virt; addr < (rng->virt + rng->size); addr += OBOS_PAGE_SIZE)
    {
        page_info info = {};
        MmS_QueryPageInfo(Mm_KernelContext.pt, addr, &info, nullptr);
        MmS_SetPageMapping(target_user->pt, &info, info.phys, false);
    }
    return base;
}

void* Mm_QuickVMAllocate(size_t sz, bool non_pageable)
{
    if (sz % OBOS_PAGE_SIZE)
        sz += (OBOS_PAGE_SIZE-(sz%OBOS_PAGE_SIZE));

    context* ctx = &Mm_KernelContext;

    irql oldIrql = Core_SpinlockAcquire(&ctx->lock);

    void* blk = MmH_FindAvailableAddress(ctx, sz, 0, nullptr);
    if (!blk)
    {
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        return nullptr;
    }

    uintptr_t base = (uintptr_t)blk;

    page_range* rng = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
    rng->ctx = ctx;
    rng->size = sz;
    rng->virt = base;
    rng->prot.present = true;
    rng->prot.rw = true;
    rng->prot.ro = false;
    rng->prot.huge_page = false;
    rng->prot.executable = false;
    rng->prot.user = false;
    rng->pageable = !non_pageable;
    memzero(&rng->working_set_nodes, sizeof(rng->working_set_nodes));

    RB_INSERT(page_tree, &ctx->pages, rng);

    for (uintptr_t addr = base; addr < (base+sz); addr += OBOS_PAGE_SIZE)
    {
        page_info info = {.prot=rng->prot,.virt=addr,.phys=0,.range=rng};

        uintptr_t phys = 0;
        if (!non_pageable)
        {
            phys = Mm_AnonPage->phys;
            MmH_RefPage(Mm_AnonPage);
            Mm_AnonPage->pagedCount++;
            Mm_AnonPage->cow_type = COW_ASYMMETRIC;
            info.prot.present = false;
            info.prot.rw = false;
        }
        else
        {
            page* pg = MmH_PgAllocatePhysical(false,false);
            if (!pg)
            {
                RB_REMOVE(page_tree, &ctx->pages, rng);
                for (uintptr_t jaddr = base; jaddr < addr; jaddr += OBOS_PAGE_SIZE)
                {
                    page_info info = {};
                    MmS_QueryPageInfo(ctx->pt, jaddr, &info, nullptr);
                    page key = {.phys=info.phys};
                    Core_MutexAcquire(&Mm_PhysicalPagesLock);
                    page* curr_pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &key);
                    Core_MutexRelease(&Mm_PhysicalPagesLock);
                    curr_pg->pagedCount--;
                    MmH_DerefPage(curr_pg);
                    info.prot.present = false;
                    MmS_SetPageMapping(ctx->pt, &info, 0, true);
                }
                Free(Mm_Allocator, rng, sizeof(*rng));
                Core_SpinlockRelease(&ctx->lock, oldIrql);
                return nullptr;
            }
            phys = pg->phys;
            pg->cow_type = COW_DISABLED;
            pg->pagedCount++;
        }
        info.phys = phys;

        MmS_SetPageMapping(ctx->pt, &info, phys, false);
    }

    ctx->stat.committedMemory += sz;
    Mm_GlobalMemoryUsage.committedMemory += sz;
    if (!non_pageable)
    {
        ctx->stat.pageable += sz;
        Mm_GlobalMemoryUsage.pageable += sz;
    }
    else 
    {
        ctx->stat.nonPaged += sz;
        Mm_GlobalMemoryUsage.nonPaged += sz;
    }

    Core_SpinlockRelease(&ctx->lock, oldIrql);

    return blk;
}
