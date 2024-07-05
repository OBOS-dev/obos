/*
 * oboskrnl/mm/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "irq/timer.h"
#include "mm/swap.h"
#include "utils/tree.h"
#include <int.h>
#include <memmanip.h>
#include <error.h>
#include <klog.h>

#include <mm/bare_map.h>
#include <mm/context.h>
#include <mm/init.h>
#include <mm/page.h>

#include <scheduler/cpu_local.h>

#include <irq/irql.h>

#include <locks/spinlock.h>
#include <stdint.h>

OBOS_EXCLUDE_VAR_FROM_MM static bool initialized;
OBOS_EXCLUDE_VAR_FROM_MM context Mm_KernelContext;
typedef struct 
{
    page* buf;
    size_t nNodes;
    size_t i;
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
        addr < round_up(round_down(region->addr) + region->size);
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
        pg->inWorkingSet = false;
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
OBOS_EXCLUDE_FUNC_FROM_MM void Mm_Initialize()
{
    if (Core_TimerInterfaceInitialized)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Timer interface cannot be initialized before the VMM. Status: %d.\n", OBOS_STATUS_INVALID_INIT_PHASE);
    Mm_KernelContext.lock = Core_SpinlockCreate();
    irql oldIrql = Core_SpinlockAcquireExplicit(&Mm_KernelContext.lock, IRQL_DISPATCH, true);
    Mm_KernelContext.owner = CoreS_GetCPULocalPtr()->currentThread->proc;
    Mm_KernelContext.pt = MmS_GetCurrentPageTable();
    mm_regions_udata udata = { nullptr,0, 0 };
    OBOSH_BasicMMIterateRegions(count_pages, &udata);
    obos_status status = OBOS_STATUS_SUCCESS;
    size_t sz = round_up(udata.nNodes*sizeof(page)+sizeof(basicmm_region));
    udata.nNodes += sz/sizeof(page);
    udata.i = 0;
    udata.buf = OBOS_BasicMMAllocatePages(udata.nNodes*sizeof(page), &status); 
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate node buffer. Status: %d.\n", status);
    OBOSH_BasicMMIterateRegions(register_pages, &udata);
    page* i = nullptr;
    RB_FOREACH(i, page_tree, &Mm_KernelContext.pages)
        if (i->pageable && false)
            Mm_SwapOut(i);
    Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
    initialized = true;
}
OBOS_EXCLUDE_FUNC_FROM_MM bool Mm_IsInitialized()
{
    return initialized;
}