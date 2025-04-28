/*
 * oboskrnl/mm/mm_sys.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <memmanip.h>
#include <klog.h>
#include <error.h>
#include <handle.h>

#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <locks/spinlock.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/page.h>
#include <mm/mm_sys.h>
#include <mm/fork.h>

#include <allocators/base.h>

#define context_from_handle(hnd, return_status, return_on_failure, use_curr) \
({\
    context* result__ = nullptr;\
    if (HANDLE_TYPE(hnd) == HANDLE_TYPE_CURRENT && use_curr)\
        result__ = Core_GetCurrentThread()->proc->ctx;\
        else {\
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());\
            obos_status status = OBOS_STATUS_SUCCESS;\
            handle_desc* _thr = OBOS_HandleLookup(OBOS_CurrentHandleTable(), (hnd), HANDLE_TYPE_VMM_CONTEXT, false, &status);\
            if (obos_is_error(status))\
            {\
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());\
                return return_status ? (__typeof__((return_on_failure)))status : (return_on_failure);\
            }\
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());\
            result__ = _thr->un.vmm_context;\
        }\
        result__;\
})
void* Sys_VirtualMemoryAlloc(handle ctx, void* base, size_t size, struct vma_alloc_userspace_args* pArgs, obos_status* pstatus)
{
    struct vma_alloc_userspace_args args = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    status = memcpy_usr_to_k(&args, pArgs, sizeof(args));
    if (obos_is_error(status))
    {
        if (pstatus)
            memcpy_k_to_usr(pstatus, &status, sizeof(status));
        return nullptr;
    }
    const handle hFile = args.file;
    prot_flags prot = args.prot;
    vma_flags flags = args.flags;
    fd* file = nullptr;
    if (HANDLE_TYPE(hFile) != HANDLE_TYPE_INVALID)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), hFile, HANDLE_TYPE_FD, false, &status);
        if (obos_is_error(status))
        {
            if (pstatus)
                memcpy_k_to_usr(pstatus, &status, sizeof(status));
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return nullptr;
        }
        file = desc->un.fd;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    context* vmm_ctx = context_from_handle(ctx, true, nullptr, true);
    prot |= OBOS_PROTECTION_USER_PAGE;
    prot &= ~OBOS_PROTECTION_CACHE_DISABLE;
    flags &= ~VMA_FLAGS_32BITPHYS; // userspace doesn't need this as much as kernel mode

    void* ret = Mm_VirtualMemoryAllocEx(vmm_ctx, base, size, prot, flags, file, args.offset, &status);
    if (pstatus)
        memcpy_k_to_usr(pstatus, &status, sizeof(status));
    return ret;
}
obos_status Sys_VirtualMemoryFree(handle ctx, void* base, size_t size)
{
    context* vmm_ctx = context_from_handle(ctx, false, 0, true);
    return Mm_VirtualMemoryFree(vmm_ctx, base, size);
}
obos_status Sys_VirtualMemoryProtect(handle ctx, void* base, size_t size, prot_flags newProt)
{
    context* vmm_ctx = context_from_handle(ctx, false, 0, true);
    newProt |= OBOS_PROTECTION_USER_PAGE;
    return Mm_VirtualMemoryProtect(vmm_ctx, base, size, newProt, 2);
}
obos_status Sys_VirtualMemoryLock(handle ctx, void* base, size_t size)
{
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status Sys_VirtualMemoryUnlock(handle ctx, void* base, size_t size)
{
    return OBOS_STATUS_UNIMPLEMENTED;
}

#ifndef OBOS_DEFAULT_WS_CAPACITY
#   define OBOS_DEFAULT_WS_CAPACITY (32*1024*1024)
#endif

handle Sys_MakeNewContext(size_t ws_capacity)
{
    if (!ws_capacity)
        ws_capacity = OBOS_DEFAULT_WS_CAPACITY;
    if (ws_capacity % OBOS_HUGE_PAGE_SIZE)
        ws_capacity += (OBOS_HUGE_PAGE_SIZE-(ws_capacity%OBOS_HUGE_PAGE_SIZE));
    context* ctx = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(context), nullptr);
    Mm_ConstructContext(ctx);
    ctx->workingSet.capacity = ws_capacity;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* desc = nullptr;
    handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_VMM_CONTEXT, &desc);
    desc->un.vmm_context = ctx;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    return hnd;
}
obos_status Sys_ContextExpandWSCapacity(handle ctx, size_t ws_capacity)
{
    context* vmm_ctx = context_from_handle(ctx, false, 0, true);
    irql oldIrql = Core_SpinlockAcquire(&vmm_ctx->lock);
    if (vmm_ctx->workingSet.capacity < ws_capacity)
    {
        Core_SpinlockRelease(&vmm_ctx->lock, oldIrql);
        return OBOS_STATUS_SUCCESS;
    }
    vmm_ctx->workingSet.capacity = ws_capacity;
    Core_SpinlockRelease(&vmm_ctx->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Sys_ContextGetStat(handle ctx, memstat* stat)
{
    if (HANDLE_TYPE(ctx) == HANDLE_TYPE_INVALID)
        return memcpy_k_to_usr(stat, &Mm_GlobalMemoryUsage, sizeof(memstat));
    context* vmm_ctx = context_from_handle(ctx, false, 0, true);
    irql oldIrql = Core_SpinlockAcquire(&vmm_ctx->lock);
    obos_status status = memcpy_k_to_usr(stat, &vmm_ctx->stat, sizeof(memstat));
    Core_SpinlockRelease(&vmm_ctx->lock, oldIrql);
    return status;
}

size_t Sys_GetUsedPhysicalMemoryCount()
{
    return Mm_PhysicalMemoryUsage;
}

obos_status Sys_QueryPageInfo(handle ctx, void* base, page_info* info)
{
    context* vmm_ctx = context_from_handle(ctx, false, 0, true);
    // Search the page in the RB-Tree.
    irql oldIrql = Core_SpinlockAcquire(&vmm_ctx->lock);
    page_range what = {.virt=(uintptr_t)base};
    page_range* rng = RB_FIND(page_tree, &vmm_ctx->pages, &what);
    if (!rng)
    {
        Core_SpinlockRelease(&vmm_ctx->lock, oldIrql);
        return OBOS_STATUS_PAGE_FAULT;
    }
    Core_SpinlockRelease(&vmm_ctx->lock, oldIrql);
    page_info tmp = {};
    MmS_QueryPageInfo(vmm_ctx->pt, what.virt, nullptr, &tmp.phys);
    tmp.virt = what.virt - (what.virt % (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE));
    tmp.prot = rng->prot;
    return memcpy_k_to_usr(info, &tmp, sizeof(tmp));
}

