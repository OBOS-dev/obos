/*
 * oboskrnl/mm/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <error.h>
#include <klog.h>

#include <mm/bare_map.h>
#include <mm/context.h>
#include <mm/init.h>
#include <mm/page.h>
#include <mm/swap.h>
#include <mm/alloc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

#include <irq/irql.h>
#include <irq/timer.h>

#include <locks/spinlock.h>

#include <utils/tree.h>

#include <allocators/basic_allocator.h>
#include <allocators/base.h>

static bool initialized;
context Mm_KernelContext;
typedef struct 
{
    page* buf;
    size_t nNodes;
    size_t i;
    size_t szPageablePages;
} mm_regions_udata;
#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
static bool count_pages(basicmm_region* region, void* udatablk)
{
    OBOS_ASSERT(udatablk);
    if (region->addr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
        return true;
    mm_regions_udata* udata = (mm_regions_udata*)udatablk;
    if (region->size < OBOS_HUGE_PAGE_SIZE)
    {
        udata->nNodes += region->size / OBOS_PAGE_SIZE;
        return true;
    }
    page pg;
    memzero(&pg, sizeof(pg));
    for (uintptr_t addr = round_down(region->addr); 
        addr < round_up(round_down(region->addr) + region->size);
        (udata->nNodes)++)
    {
        MmS_QueryPageInfo(MmS_GetCurrentPageTable(), addr, &pg);
        if (pg.prot.huge_page)
            addr += OBOS_HUGE_PAGE_SIZE;
        else
            addr += OBOS_PAGE_SIZE;
    }
    return true;
}
static bool register_pages(basicmm_region* region, void* udatablk)
{
    OBOS_ASSERT(udatablk);
    if (region->addr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
        return true;
    mm_regions_udata* udata = (mm_regions_udata*)udatablk;
    for (uintptr_t addr = round_down(region->addr); 
        addr < round_down(region->addr) + region->size;
        )
    {
        OBOS_ASSERT(udata->i++ < udata->nNodes);
        page *pg = &udata->buf[udata->i - 1];
        memzero(pg, sizeof(*pg));
        MmS_QueryPageInfo(MmS_GetCurrentPageTable(), addr, pg);
        pg->addr = addr;
        pg->pageable = !(MmH_IsAddressUnPageable(addr) ||
                       ((addr >= round_down(udata->buf)) && (addr < round_up(&udata->buf[udata->nNodes]))) ||
                       region->mmioRange); 
        if (pg->pageable)
            udata->szPageablePages += (pg->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        pg->workingSets = 0;
        pg->pagedOut = false;
        pg->age = 0;
        pg->owner = &Mm_KernelContext;
        RB_INSERT(page_tree, &Mm_KernelContext.pages, pg);
        if (pg->prot.huge_page)
            addr += OBOS_HUGE_PAGE_SIZE;
        else
            addr += OBOS_PAGE_SIZE;
    }
    return true;
}
static basic_allocator non_paged_pool_alloc;
static basic_allocator vmm_alloc;
void Mm_Initialize()
{
    OBOSH_ConstructBasicAllocator(&non_paged_pool_alloc);
    OBOSH_ConstructBasicAllocator(&vmm_alloc);
    OBOS_NonPagedPoolAllocator = (allocator_info*)&non_paged_pool_alloc;
    Mm_Allocator = (allocator_info*)&vmm_alloc;
    // if (Core_TimerInterfaceInitialized)
    //     OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Timer interface cannot be initialized before the VMM. Status: %d.\n", __func__, OBOS_STATUS_INVALID_INIT_PHASE);
    Mm_KernelContext.lock = Core_SpinlockCreate();
    irql oldIrql = Core_SpinlockAcquireExplicit(&Mm_KernelContext.lock, IRQL_DISPATCH, true);
    Mm_KernelContext.owner = CoreS_GetCPULocalPtr()->currentThread->proc;
    Mm_KernelContext.pt = MmS_GetCurrentPageTable();
    CoreS_GetCPULocalPtr()->currentThread->proc->ctx = &Mm_KernelContext;
    // CoreS_GetCPULocalPtr()->currentContext = &Mm_KernelContext;
    for (size_t i = 0; i < Core_CpuCount; i++)
        Core_CpuInfo[i].currentContext = &Mm_KernelContext;
    mm_regions_udata udata = { };
    OBOSH_BasicMMIterateRegions(count_pages, &udata);
    obos_status status = OBOS_STATUS_SUCCESS;
    size_t sz = round_up(udata.nNodes*sizeof(page)+sizeof(basicmm_region));
    udata.nNodes += sz/sizeof(page);
    udata.i = 0;
    udata.buf = OBOS_BasicMMAllocatePages(udata.nNodes*sizeof(page), &status); 
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate node buffer. Status: %d.\n", status);
    OBOSH_BasicMMIterateRegions(register_pages, &udata);
    // Do any architecture-specific rounding here.
#if defined(__x86_64__) || defined(__m68k__)
    udata.szPageablePages = (udata.szPageablePages + 0x3fff) & ~0x3fff;
#endif
    // Mm_KernelContext.workingSet.capacity = udata.szPageablePages;
    Mm_KernelContext.workingSet.capacity = 0x20000 /* 128 KiB */;
    initialized = true;
    page* i = nullptr;
    RB_FOREACH(i, page_tree, &Mm_KernelContext.pages)
        if (i->pageable)
            Mm_SwapOut(i);
    Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
}
bool Mm_IsInitialized()
{
    return initialized;
}