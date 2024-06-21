/*
 * oboskrnl/mm/init.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <error.h>
#include <klog.h>

#include <mm/context.h>
#include <mm/init.h>
#include <mm/bare_map.h>
#include <mm/pg_node.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <allocators/basic_allocator.h>
#include <allocators/base.h>

#include <utils/tree.h>

context* Mm_KernelContext;

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

static basic_allocator allocator;
struct register_mm_regions_udata
{   
    page_node* node_buf;
    size_t i;
};
static bool register_mm_regions(basicmm_region* region, void* udata_)
{
    struct register_mm_regions_udata* udata = (struct register_mm_regions_udata*)udata_;
    pt_context ctx = MmS_PTContextGetCurrent();
    for (uintptr_t addr = region->addr & ~0xfff; addr < ((region->addr & ~0xfff) + region->size); )
    {
        page_node* node = &udata->node_buf[udata->i++]; 
        pt_context_page_info info;
        memzero(&info, sizeof(info));
        MmS_PTContextQueryPageInfo(&ctx, addr, &info);
        node->addr = addr;
        node->huge_page = info.huge_page;
        node->accessed = info.accessed;
        node->dirty = info.dirty;
        node->present = info.present;
        node->protection = info.protection;
        RB_INSERT(page_node_tree, &Mm_KernelContext->pageNodeTree, node);
        if (info.huge_page)
            addr += OBOS_HUGE_PAGE_SIZE;
        else
            addr += OBOS_PAGE_SIZE;
    }
    return true;
}
static bool get_page_node_buf_count(basicmm_region* region, void* udata_)
{
    size_t* sz = (size_t*)udata_;
    if (region->size < OBOS_HUGE_PAGE_SIZE)
    {
        (*sz) += region->size/OBOS_PAGE_SIZE;
        return true;
    }
    else
    {
        pt_context ctx = MmS_PTContextGetCurrent();
        for (uintptr_t addr = region->addr & ~0xfff; addr < ((region->addr & ~0xfff) + region->size); (*sz)++)
        {
            pt_context_page_info info;
            memzero(&info, sizeof(info));
            MmS_PTContextQueryPageInfo(&ctx, addr, &info);
            if (info.huge_page)
                addr += OBOS_HUGE_PAGE_SIZE;
            else
                addr += OBOS_PAGE_SIZE;
        }
    }
    return true;
}
void Mm_Initialize()
{
    OBOS_ASSERT(obos_unlikely_error(OBOSH_ConstructBasicAllocator(&allocator)));
    Mm_VMMAllocator = (allocator_info*)&allocator;
    OBOS_ASSERT(!Mm_KernelContext);
    obos_status status = OBOS_STATUS_SUCCESS;
    Mm_KernelContext = MmH_AllocateContext(&status);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a vmm context object for the kernel. Status: %d\n", status);
    process* current = Core_GetCurrentThread()->proc;
    status = MmH_InitializeContext(Mm_KernelContext, current);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the vmm context object for the kernel. Status: %d\n", status);
    Mm_KernelContext->pt_ctx = MmS_PTContextGetCurrent();
    size_t nNodes = 0;
    OBOSH_BasicMMIterateRegions(get_page_node_buf_count, &nNodes);
    nNodes += (((nNodes*sizeof(page_node)+sizeof(basicmm_region)) + 0xfff) & ~0xfff) / 0x1000;
    page_node* node_buf = OBOS_BasicMMAllocatePages(nNodes*sizeof(page_node), &status);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a page node buffer. Status: %d\n", status);
    struct register_mm_regions_udata udata = {
        .node_buf = node_buf,
        .i = 0,
    };
    OBOSH_BasicMMIterateRegions(register_mm_regions, &udata);
}