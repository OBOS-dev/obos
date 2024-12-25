/*
 * oboskrnl/locks/mutex.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "wait.h"
#include <int.h>
#include <klog.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <irq/irql.h>
#include <irq/timer.h>

#include <locks/mutex.h>
#include <locks/spinlock.h>
#include <locks/wait.h>

#include <stdatomic.h>

#ifdef __x86_64__
#	define spinlock_hint() __builtin_ia32_pause()
#elif defined(__m68k__)
#	define spinlock_hint() asm("nop")
#endif

obos_status Core_MutexAcquire(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // oops
    // if (mut->who == Core_GetCurrentThread())
    //     return OBOS_STATUS_RECURSIVE_LOCK;
    OBOS_ASSERT(Core_GetIrql() <= IRQL_DISPATCH);
    if (Core_GetIrql() > IRQL_DISPATCH)
        return OBOS_STATUS_INVALID_IRQL;
    OBOS_ASSERT(mut->who != Core_GetCurrentThread());
    // Spin for a bit.
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    int spin = 100000;
    bool success = true;
    while (atomic_flag_test_and_set_explicit(&mut->lock, memory_order_seq_cst) && success)
    {
        spinlock_hint();
        if (mut->ignoreAllAndBlowUp)
        {
            if (oldIrql != IRQL_INVALID)
                Core_LowerIrql(oldIrql);
            return OBOS_STATUS_ABORTED;
        }
        if (spin-- >= 0)
            success = false;
    }
    if (mut->ignoreAllAndBlowUp)
    {
        if (oldIrql != IRQL_INVALID)
            Core_LowerIrql(oldIrql);
        return OBOS_STATUS_ABORTED;
    }
    if (oldIrql != IRQL_INVALID)
        Core_LowerIrql(oldIrql);
    if (success)
    {
        mut->who = Core_GetCurrentThread();
        mut->locked = true;
        mut->lastLockTimeNS = CoreH_TickToNS(CoreS_GetNativeTimerTick(), true);
        return OBOS_STATUS_SUCCESS;
    }
    printf("tid %d: waiting for tid %d to release mutex %p\n", Core_GetCurrentThread()->tid, mut->who->tid, mut);
    obos_status st = Core_WaitOnObject(&mut->hdr);
    if (st != OBOS_STATUS_SUCCESS) {
        return st;
    }
    if (mut->ignoreAllAndBlowUp)
        return OBOS_STATUS_ABORTED;
    while (atomic_flag_test_and_set_explicit(&mut->lock, memory_order_seq_cst) && success)
        spinlock_hint();
    mut->who = Core_GetCurrentThread();
#if OBOS_ENABLE_LOCK_PROFILING
    mut->lastLockTimeNS = CoreH_TickToNS(CoreS_GetNativeTimerTick(), true);
#endif
    mut->locked = true;
    return OBOS_STATUS_SUCCESS;
}
obos_status Core_MutexTryAcquire(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (mut->locked)
        return OBOS_STATUS_IN_USE;
    return Core_MutexAcquire(mut);
}
obos_status Core_MutexRelease(mutex* mut)
{
    if (!mut)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!mut->locked)
        return OBOS_STATUS_SUCCESS;
    if (mut->who != Core_GetCurrentThread())
        return OBOS_STATUS_ACCESS_DENIED;
    mut->who = nullptr;
#if OBOS_ENABLE_LOCK_PROFILING
    mut->lastLockTimeNS = CoreH_TickToNS(CoreS_GetNativeTimerTick(), true) - mut->lastLockTimeNS;
#endif
    atomic_flag_clear_explicit(&mut->lock, memory_order_seq_cst);
    obos_status status = CoreH_SignalWaitingThreads(&mut->hdr, false, false);
    if (obos_is_error(status))
        return status;
    mut->locked = false;
    return OBOS_STATUS_SUCCESS;
}
bool Core_MutexAcquired(mutex* mut)
{
    if (!mut)
        return false;
    return mut->locked;
}
