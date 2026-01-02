/*
 * oboskrnl/mm/handler.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <mm/handler.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/swap.h>
#include <mm/pmm.h>
#include <mm/bare_map.h>
#include <mm/alloc.h>

#include <vfs/vnode.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <locks/spinlock.h>
#include <locks/mutex.h>

#include <vfs/pagecache.h>

#include <allocators/base.h>

obos_status Mm_AgingPRA(context* ctx);
obos_status Mm_AgingReferencePage(context* ctx, working_set_node* node);

static void map_file_region(page_range* rng, uintptr_t addr, uint32_t ec, fault_type *type, page_info *info)
{
    if (!rng->prot.rw && ec & PF_EC_RW)
    {
        *type = ACCESS_FAULT;
        return;
    }
    page what = {.backing_vn=rng->un.mapped_vn,.file_offset = rng->base_file_offset + (addr-rng->virt)};
    page* phys = RB_FIND(pagecache_tree, &rng->un.mapped_vn->cache, &what);
    if (!phys)
    {
        *type = HARD_FAULT;
        phys = VfsH_PageCacheCreateEntry(rng->un.mapped_vn, what.file_offset);
    }
    else
        *type = SOFT_FAULT;
    if (!phys)
    {
        *type = ACCESS_FAULT;
        return;
    }
    irql oldIrql = Core_SpinlockAcquire(&rng->ctx->lock);
    MmH_RefPage(phys);
    if (ec & PF_EC_RW)
        Mm_MarkAsDirtyPhys(phys);
    info->phys = phys->phys;
    info->prot.present = true;
    if (rng->priv)
    {
        info->prot.rw = false;
        phys->cow_type = COW_SYMMETRIC;
    }
    else
        info->prot.rw = rng->prot.rw;
    
    phys->pagedCount++;
    MmS_SetPageMapping(rng->ctx->pt, info, phys->phys, false);
    MmS_TLBShootdown(rng->ctx->pt, info->virt, info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    Core_SpinlockRelease(&rng->ctx->lock, oldIrql);
}

static bool sym_cow_cpy(context* ctx, page_range* rng, uintptr_t addr, uint32_t ec, page** pg, page_info* info)
{
    OBOS_UNUSED(addr && ec);
    info->prot.present = true;
    if ((*pg)->refcount == 1 /* we're the only one left */)
    {
        // Steal the page.
        info->prot.rw = true;
        info->prot.ro = false;
        MmS_SetPageMapping(ctx->pt, info, (*pg)->phys, false);
        MmS_TLBShootdown(ctx->pt, info->virt, info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        (*pg)->cow_type = COW_DISABLED;
        return true;
    }
    page* new = MmH_PgAllocatePhysical(rng->phys32, info->prot.huge_page);
    if (!new)
        return false;
    new->pagedCount++;
    memcpy(MmS_MapVirtFromPhys(new->phys), MmS_MapVirtFromPhys((*pg)->phys), info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    info->prot.rw = true;
    info->prot.ro = false;
    MmS_SetPageMapping(ctx->pt, info, new->phys, false);
    MmS_TLBShootdown(ctx->pt, info->virt, info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    (*pg)->pagedCount--;
    MmH_DerefPage((*pg));
    (*pg) = new;
    return true;
}

static obos_status ref_page(context* ctx, const page_info *curr)
{
    page_range* const volatile rng = curr->range;
    obos_status status = OBOS_STATUS_SUCCESS;
    working_set_entry* ent = nullptr;
    bool allocated_ent = false;
    if (!ent)
    {
        ent = ZeroAllocate(Mm_Allocator, 1, sizeof(working_set_entry), nullptr);
        ent->info.virt = curr->virt;
        ent->info.prot = curr->prot;
        ent->info.range = rng;
        allocated_ent = true;
    }
    ent->refs++;
    working_set_node* node = ZeroAllocate(Mm_Allocator, 1, sizeof(working_set_node), nullptr);
    node->data = ent;
#if defined(OBOS_PAGE_REPLACEMENT_AGING)
    status = Mm_AgingReferencePage(ctx, node);
#else
#   error No page replacement algorithm defined at compile time. This is a bug.
#endif
    if (obos_is_error(status))
    {
        Free(Mm_Allocator, node, sizeof(*node));
        if (allocated_ent)
            Free(Mm_Allocator, ent, sizeof(*ent));
        return status;
    }
    // TODO: Try to figure out a better number based off the count of pages in the context/working-set.
    const size_t threshold = 512;
    if (ctx->referenced.nNodes >= threshold)
        status = Mm_RunPRA(ctx);
    return status;
}

static bool asym_cow_cpy(context* ctx, page_range* rng, uintptr_t addr, uint32_t ec, page** pg, page_info* info, irql *oldIrql)
{
    OBOS_UNUSED(addr);
    info->prot.present = true;
    info->prot.rw = false;
    info->prot.ro = true;
    if (ec & PF_EC_RW)
    {
        if (rng->prot.ro)
            return false; // whoops
        if ((*pg)->refcount == 1 /* we're the only one left */)
        {
            // Steal the page.
            info->prot.rw = true;
            info->prot.ro = false;
            (*pg)->cow_type = COW_DISABLED;
            goto done;
        }
        Core_SpinlockRelease(&ctx->lock, *oldIrql);
        page* new = MmH_PgAllocatePhysical(rng->phys32, info->prot.huge_page);
        *oldIrql = Core_SpinlockAcquire(&ctx->lock);
        if (!new)
        {
            OBOS_Warning("%s: MmH_PgAllocatePhysical returned nullptr (OOM)\n", __func__);
            return false;
        }
        new->pagedCount++;
        memcpy(MmS_MapVirtFromPhys(new->phys), MmS_MapVirtFromPhys((*pg)->phys), info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        info->prot.rw = true;
        info->prot.ro = false;
        (*pg)->pagedCount--;
        MmH_DerefPage((*pg));
        *pg = new;
    }
    done:
    MmS_SetPageMapping(ctx->pt, info, (*pg)->phys, false);
    MmS_TLBShootdown(rng->ctx->pt, info->virt, info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    info->range = rng;
    // if (ec & PF_EC_RW)
    //     Mm_MarkAsDirtyPhys(pg);
    // else
    //     Mm_MarkAsStandbyPhys(pg);
    MmS_QueryPageInfo(rng->ctx->pt, info->virt, info, nullptr);
    ref_page(ctx, info);
    return true;
}

obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec)
{
    OBOS_ASSERT(ctx);
    if (~ec & PF_EC_UM)
        ctx = &Mm_KernelContext;
    bool handled = false;
    page_range what = {.virt=addr,.size=OBOS_PAGE_SIZE};
    // if (ctx != &Mm_KernelContext)
    //     OBOS_Debug("(pid %d) Handling page fault at 0x%p...\n", ctx->owner->pid, addr);
    fault_type type = INVALID_FAULT;
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
    {
        OBOS_Debug("Fatal Paqe Fault: No page range found for target at 0x%p\n", what.virt);
        goto done;
    }
    page_info curr = {};
    MmS_QueryPageInfo(ctx->pt, addr, &curr, nullptr);
    page* pg = nullptr;
    do
    {
        page what = {.phys=curr.phys};
        Core_MutexAcquire(&Mm_PhysicalPagesLock);
        pg = (curr.phys && !curr.prot.is_swap_phys) ? RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what) : nullptr;
        Core_MutexRelease(&Mm_PhysicalPagesLock);
        if (!pg && !rng->un.mapped_vn && !curr.prot.is_swap_phys)
        {
            OBOS_Debug("No physical page found for virtual page %p (curr.phys: %p, found nothing)\n", curr.virt, curr.phys);
            goto done;
        }
    } while(0);
    curr.range = rng;
    curr.prot.user = ec & PF_EC_UM;
    // CoW regions are not file mappings (directly, at least; private file mappings are CoW).
    if (rng->un.mapped_vn)
    {
        if (ctx != &Mm_KernelContext)
            OBOS_Debug("Trying file mapping...\n", addr);
        // page_info info = {};
        // MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        handled = true;
        fault_type curr_type = SOFT_FAULT;
        if (~ec & PF_EC_PRESENT)
            map_file_region(rng, addr, ec, &curr_type, &curr);
        else
            handled = false;
        if (curr_type > type && handled)
            type = curr_type;
        // if (type == INVALID_FAULT)
        //     handled = true;
    }
    if (!handled && pg && pg->cow_type)
    {
        // if (ctx != &Mm_KernelContext)
        //     OBOS_Debug("Doing CoW...\n", addr);
        // Mooooooooo.
        irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
        switch (pg->cow_type) {
            case COW_SYMMETRIC:
                handled = sym_cow_cpy(ctx, rng, addr, ec, &pg, &curr);
                break;
            case COW_ASYMMETRIC:
                handled = asym_cow_cpy(ctx, rng, addr, ec, &pg, &curr, &oldIrql);
                break;
            default:
                handled = false;
                break;
        }
        fault_type curr_type = SOFT_FAULT;
        if (curr_type > type && handled)
            type = curr_type;
        Core_SpinlockRelease(&ctx->lock, oldIrql);
        goto done;
    }
    if (!handled && curr.prot.is_swap_phys)
    {
        if (ctx != &Mm_KernelContext)
            OBOS_Debug("Trying a swap in of 0x%p...\n", addr);
        // Try a swap in?
        fault_type curr_type = SOFT_FAULT;
        // for (volatile bool b = !rng->pageable; b; )
        //     ;
        obos_status status = Mm_SwapIn(&curr, &curr_type);
        if (curr_type > type)
            type = curr_type;
        if (obos_is_error(status))
            goto done;
        irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
        ctx->stat.paged -= (curr.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        Mm_GlobalMemoryUsage.paged -= (curr.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        MmS_QueryPageInfo(ctx->pt, addr, &curr, nullptr);
        ref_page(ctx, &curr);
        handled = true;
        Core_SpinlockRelease(&ctx->lock, oldIrql);
    }
    done:
    if (!handled && type == INVALID_FAULT)
        type = ACCESS_FAULT;
    if (type == ACCESS_FAULT)
        handled = false;
    if (type == ACCESS_FAULT && rng)
        if (rng->hasGuardPage && (rng->virt==addr))
            OBOS_Debug("Page fault happened on guard page. Stack overflow possible\n");
    ctx->stat.pageFaultCount++;
    ctx->stat.pageFaultCountSinceSample++;
    switch (type) {
        case SOFT_FAULT:
            ctx->stat.softPageFaultCount++;
            ctx->stat.softPageFaultCountSinceSample++;
            break;
        case HARD_FAULT:
            ctx->stat.hardPageFaultCount++;
            ctx->stat.hardPageFaultCountSinceSample++;
            break;
        case ACCESS_FAULT:
            break;
        default:
            OBOS_ASSERT(!"invalid fault type. fault is neither a SOFT_FAULT, HARD_FAULT, nor a ACCESS_FAULT.");
            break;
    }
    // OBOS_Debug("Page fault went %shandled\n", handled ? "" : "un");
    return !handled ? OBOS_STATUS_UNHANDLED : OBOS_STATUS_SUCCESS;
}

obos_status Mm_RunPRA(context* ctx)
{
    OBOS_ASSERT(ctx);

    ctx->stat.pageFaultCountSinceSample = 0;
    ctx->stat.hardPageFaultCountSinceSample = 0;
    ctx->stat.softPageFaultCountSinceSample = 0;
    obos_status status = OBOS_STATUS_SUCCESS;
#if defined(OBOS_PAGE_REPLACEMENT_AGING)
    status = Mm_AgingPRA(ctx);
#else
#   error No page replacement algorithm defined at compile time. This is a bug.
#endif

    // done:
    return status;
}

void MmH_RemovePageFromWorkingset(context* ctx, working_set_node* node)
{
    working_set_entry* const ent = node->data;
    REMOVE_WORKINGSET_PAGE_NODE(ctx->workingSet.pages, node);
    node->prev = nullptr;
    node->next = nullptr;
    if (!(--ent->workingSets))
    {
        if (!ent->free)
        {
            if (obos_is_success(Mm_SwapOut(ent->info.virt, ctx)))
            {
                ctx->stat.paged += (ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
                Mm_GlobalMemoryUsage.paged += (ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
            }
        }
        Free(Mm_Allocator, ent, sizeof(*ent));
    }
    Free(Mm_Allocator, node, sizeof(*node));
}
