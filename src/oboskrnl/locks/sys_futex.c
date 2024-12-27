/*
 * oboskrnl/locks/sys_futex.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>

#include <allocators/base.h>

#include <mm/context.h>
#include <mm/pmm.h>

#include <locks/sys_futex.h>
#include <locks/mutex.h>
#include <locks/wait.h>

#include <stdatomic.h>

#include <utils/shared_ptr.h>
#include <utils/tree.h>

RB_GENERATE(futex_tree, futex, node, cmp_futex);

futex_tree futexes = RB_INITIALIZER(futex_tree);
// tree lock
mutex futexes_lock = MUTEX_INITIALIZE();

static futex_object* find_futex(uint32_t* obj, bool create)
{
    futex_object key = {.obj=obj};
    Core_MutexAcquire(&futexes_lock);
    futex_object* ret = RB_FIND(futex_tree, &futexes, &key);
    if (!ret && create)
    {
        ret = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(futex_object), nullptr);
        ret->obj = obj;
        ret->wait_hdr = WAITABLE_HEADER_INITIALIZE(false, false);
        ret->ctx = CoreS_GetCPULocalPtr()->currentContext;
        RB_INSERT(futex_tree, &futexes, ret);
    }
    Core_MutexRelease(&futexes_lock);

    if (ret)
        ret->refs++;
    return ret;
}
static void deref_futex(futex_object* fut)
{
    if (!fut) return;
    if (!(--fut->refs))
    {
        RB_REMOVE(futex_tree, &futexes, fut);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, fut, sizeof(*fut));
    }
}

obos_status Sys_FutexWait(uint32_t *futex, uint32_t cmp_with, uint64_t timeout)
{
    // TODO: Timeout.
    OBOS_UNUSED(timeout);

    // Check alignment.
    if ((uintptr_t)futex & 0b11)
        return OBOS_STATUS_INVALID_ARGUMENT;


    // TODO: F****** implement virtual page locking
    // Mm_VirtualMemoryLock(CoreS_GetCPULocalPtr()->currentThread->proc->ctx, futex, sizeof(*futex));

    uintptr_t phys = 0;
    page_info info = {};
    MmS_QueryPageInfo(CoreS_GetCPULocalPtr()->currentThread->proc->ctx->pt, (uintptr_t)futex, &info, nullptr);
    phys = info.phys;
    _Atomic(uint32_t)* val = MmS_MapVirtFromPhys(phys+((uintptr_t)futex % (info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)));

    uint32_t expected = 0;
    bool cmp_val =
        atomic_compare_exchange_strong(
            val,
            &expected,
            cmp_with);
    // Only create a futex if we're going to be waiting on the futex, otherwise it's wasted time
    futex_object* obj = find_futex(futex, cmp_val);
    if (cmp_val)
    {
        // Mm_VirtualMemoryUnlock(CoreS_GetCPULocalPtr()->currentThread->proc->ctx, futex, sizeof(*futex));
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(*obj));
        deref_futex(obj);
        return status;
    }

    // Mm_VirtualMemoryUnlock(CoreS_GetCPULocalPtr()->currentThread->proc->ctx, futex, sizeof(*futex));
    return OBOS_STATUS_RETRY /* linux does this, so we will too */;
}

obos_status Sys_FutexWake(uint32_t *futex, uint32_t nWaiters)
{
    // Check alignment.
    if ((uintptr_t)futex & 0b11)
        return OBOS_STATUS_INVALID_ARGUMENT;

    futex_object* obj = find_futex(futex, false);

    if (nWaiters == UINT32_MAX)
        CoreH_SignalWaitingThreads(WAITABLE_OBJECT(*obj), true, false); // wake everyone
    else
        for (uint32_t i = 0; i < nWaiters; i++)
            CoreH_SignalWaitingThreads(WAITABLE_OBJECT(*obj), false, false); // only wake nWaiters

    return OBOS_STATUS_SUCCESS;
}
