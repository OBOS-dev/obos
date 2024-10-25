/*
 * oboskrnl/mm/handler.c
 *
 * Copyright (c) 2024 Omar Berrow
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

#include <vfs/pagecache.h>
#include <vfs/vnode.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <locks/spinlock.h>
#include <locks/mutex.h>

obos_status Mm_AgingPRA(context* ctx);
obos_status Mm_AgingReferencePage(context* ctx, working_set_node* node);

static void mark_file_region_dirty(page_range* rng, uintptr_t addr, uint32_t ec)
{
    OBOS_UNUSED(ec);
    context* const ctx = rng->ctx;
    uintptr_t fileoff = addr-rng->virt + rng->mapped_here->fileoff;
    vnode* vn = (vnode*)rng->mapped_here->owner->owner;
    size_t sz = OBOS_PAGE_SIZE;
    if (rng->mapped_here->fileoff+fileoff+sz > vn->filesize)
        sz = (rng->mapped_here->fileoff+fileoff+sz) % OBOS_PAGE_SIZE;
    VfsH_PCDirtyRegionCreate(&vn->pagecache, fileoff, sz);
    uintptr_t phys = 0;
    MmS_QueryPageInfo(ctx->pt, addr, nullptr, &phys);
    page_info curr = {};
    curr.virt = addr;
    curr.phys = phys;
    curr.range = rng;
    curr.prot = rng->prot;
    curr.prot.rw = true;
    curr.prot.present = true;
    MmS_SetPageMapping(ctx->pt, &curr, phys, false);
}
static void map_file_region(page_range* rng, uintptr_t addr, uint32_t ec, fault_type *type)
{
    context* const ctx = rng->ctx;
    const uintptr_t fileoff = addr-rng->virt + rng->mapped_here->fileoff;
    vnode* vn = (vnode*)rng->mapped_here->owner->owner;
    size_t sz = OBOS_PAGE_SIZE;
    if (fileoff+sz > vn->filesize)
        sz = (vn->filesize-fileoff);
    void* pc_base = VfsH_PageCacheGetEntry(rng->mapped_here->owner, fileoff, sz, type);
    uintptr_t phys = 0;
    MmS_QueryPageInfo(ctx->pt, (uintptr_t)pc_base, nullptr, &phys);
    page_info curr = {};
    curr.virt = addr;
    curr.phys = phys;
    curr.range = rng;
    curr.prot = rng->prot;
    curr.prot.rw = (ec & PF_EC_RW) && !rng->prot.ro;
    page what = {.phys=phys};

    page* phys_pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    MmH_RefPage(phys_pg);

    curr.prot.present = true;
    MmS_SetPageMapping(ctx->pt, &curr, phys, true);
    if (curr.prot.rw)
        VfsH_PCDirtyRegionCreate(&vn->pagecache, fileoff, sz);
}
static bool sym_cow_cpy(context* ctx, page_range* rng, uintptr_t addr, uint32_t ec, page_info* info) 
{
    if (~ec & PF_EC_PRESENT)
        return false;
    if (rng->prot.ro)
            return false; // whoops
    page what = {.phys=info->phys};
    page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    if (pg->refcount == 1 /* we're the only one left */)
    {
        // Steal the page.
        info->prot.rw = true;
        info->prot.ro = false;
        MmS_SetPageMapping(ctx->pt, info, pg->phys, false);
        return true;
    }
    page* new = MmH_PgAllocatePhysical(rng->phys32, info->prot.huge_page);
    memcpy(MmS_MapVirtFromPhys(new->phys), MmS_MapVirtFromPhys(pg->phys), info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    info->prot.rw = true;
    info->prot.ro = false;
    MmS_SetPageMapping(ctx->pt, info, pg->phys, false);
    MmH_DerefPage(pg);
    return true;
}
static obos_status ref_page(context* ctx, page_info *curr)
{
    page_range* const rng = curr->range;
    obos_status status = OBOS_STATUS_SUCCESS;
    working_set_entry* ent = nullptr;
    // for (working_set_node* node = rng->working_set_nodes.head; node; )
    // {
    //     if (node->data->info.virt == addr)
    //     {
    //         ent = node->data;
    //         break;
    //     }
    //     node = node->next;
    // }
    bool allocated_ent = false;
    if (!ent)
    {
        ent = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(working_set_entry), nullptr);
        ent->info = *curr;
        allocated_ent = true;
    }
    ent->refs++;
    working_set_node* node = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(working_set_node), nullptr);
    node->data = ent;
#if defined(OBOS_PAGE_REPLACEMENT_AGING)
    status = Mm_AgingReferencePage(ctx, node);
#else
#   error No page replacement algorithm defined at compile time. This is a bug.
#endif
    if (obos_is_error(status))
    {
        Mm_Allocator->Free(Mm_Allocator, node, sizeof(*node));
        if (allocated_ent)
            Mm_Allocator->Free(Mm_Allocator, ent, sizeof(*ent));
        return status;
    }
    APPEND_WORKINGSET_PAGE_NODE(ctx->referenced, node);
    ent->pr_node.data = ent;
    APPEND_WORKINGSET_PAGE_NODE(rng->working_set_nodes, &ent->pr_node);
    // TODO: Try to figure out a better number based off the count of pages in the context/working-set.
    const size_t threshold = 512;
    if (ctx->referenced.nNodes >= threshold)
        status = Mm_RunPRA(ctx);
    return status;
}
static bool asym_cow_cpy(context* ctx, page_range* rng, uintptr_t addr, uint32_t ec, page_info* info) 
{
    page what = {.phys=info->phys};
    page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    info->prot.present = true;
    info->prot.rw = false;
    info->prot.ro = true;
    if (ec & PF_EC_RW)
    {
        if (rng->prot.ro)
            return false; // whoops
        if (pg->refcount == 1 /* we're the only one left */)
        {
            // Steal the page.
            info->prot.rw = true;
            info->prot.ro = false;
            goto done;
        }
        page* new = MmH_PgAllocatePhysical(rng->phys32, info->prot.huge_page);
        if (~ec & PF_EC_PRESENT)
            MmS_SetPageMapping(ctx->pt, info, pg->phys, false);
        memcpy(MmS_MapVirtFromPhys(new->phys), MmS_MapVirtFromPhys(pg->phys), info->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        info->prot.rw = true;
        info->prot.ro = false;
        MmH_DerefPage(pg);
        pg = new;
    }
    done:
    MmS_SetPageMapping(ctx->pt, info, pg->phys, false);
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
    if (ctx != &Mm_KernelContext)
        OBOS_Debug("Handling page fault at 0x%p...\n", addr);
    fault_type type = INVALID_FAULT;
    page_range* rng = RB_FIND(page_tree, &ctx->pages, &what);
    if (!rng)
        goto done;
    page_info curr = {};
    MmS_QueryPageInfo(ctx->pt, addr, &curr, nullptr);
    curr.range = rng;
    // CoW regions are not file mappings (directly, at least; private file mappings are CoW).
    if (rng->mapped_here && !rng->cow)
    {
        // page_info info = {};
        // MmS_QueryPageInfo(ctx->pt, addr, &info, nullptr);
        irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
        handled = true;
        fault_type curr_type = SOFT_FAULT;
        if (~ec & PF_EC_PRESENT)
            map_file_region(rng, addr, ec, &curr_type);
        else if (ec & PF_EC_RW && rng->prot.rw)
            mark_file_region_dirty(rng, addr, ec);
        else
            handled = false;
        if (curr_type > type && handled)
            type = curr_type;
        Core_SpinlockRelease(&ctx->lock, oldIrql);
    }
    if (rng->cow)
    {
        // Mooooooooo.
        irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
        switch (rng->un.cow_type) {
            case COW_SYMMETRIC:
                handled = sym_cow_cpy(ctx, rng, addr, ec, &curr);
                break;
            case COW_ASYMMETRIC:
                handled = asym_cow_cpy(ctx, rng, addr, ec, &curr);
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
    if (!handled && ~ec & PF_EC_PRESENT && curr.phys != 0)
    {
        // Try a swap in?
        irql oldIrql = Core_SpinlockAcquire(&ctx->lock);
        fault_type curr_type = SOFT_FAULT;
        // for (volatile bool b = !rng->pageable; b; )
        //     ;
        obos_status status = Mm_SwapIn(&curr, &curr_type);
        if (curr_type > type)
            type = curr_type;
        if (obos_is_error(status))
        {
            Core_SpinlockRelease(&ctx->lock, oldIrql);
            goto done;
        }
        ctx->stat.paged -= (curr.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        MmS_QueryPageInfo(ctx->pt, addr, &curr, nullptr);
        ref_page(ctx, &curr);
        handled = true;
        Core_SpinlockRelease(&ctx->lock, oldIrql);
    }
    done:
    if (!handled && type == INVALID_FAULT)
        type = ACCESS_FAULT;
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
        obos_status status = ent->free ? OBOS_STATUS_ABORTED : Mm_SwapOut(&ent->info);
        if (obos_is_success(status))
            ctx->stat.paged += (ent->info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        if (!ent->free)
            REMOVE_WORKINGSET_PAGE_NODE(ent->info.range->working_set_nodes, &ent->pr_node);
        Mm_Allocator->Free(Mm_Allocator, ent, sizeof(*ent));
    }
    Mm_Allocator->Free(Mm_Allocator, node, sizeof(*node));
}
