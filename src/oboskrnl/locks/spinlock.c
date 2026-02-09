/*
    oboskrnl/locks/spinlock.c

    Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <stdatomic.h>

#include <irq/irql.h>
#include <irq/timer.h>

#include <locks/spinlock.h>

#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>

static uint64_t nanoseconds_since_boot()
{
#if !OBOS_ENABLE_LOCK_PROFILING
    return 0;
#else
    if (!CoreS_GetNativeTimerFrequency())
        return 0;
    return CoreH_TickToNS(CoreS_GetNativeTimerTick(), true);
#endif
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant)
{
    // IRQL_INVALID is used to specify not to raise the irql at all
    // this way, LowerIrql can lock the DPC queue without breaking.
    irql oldIrql = IRQL_INVALID;
    if (obos_expect(Core_GetIrql() < minIrql, false))
        oldIrql = Core_RaiseIrqlNoThread(minIrql);

    if (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
        while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
            OBOSS_SpinlockHint();

#if OBOS_DEBUG
    lock->caller = __builtin_return_address(0);
#endif

    return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN irql Core_SpinlockAcquire(spinlock* const lock)
{
    irql oldIrql = IRQL_INVALID;
    if (obos_expect(Core_GetIrql() < IRQL_DISPATCH, false))
        oldIrql = Core_RaiseIrqlNoThread(IRQL_DISPATCH);
    if (!atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
       goto locked;
    while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
        OBOSS_SpinlockHint();
    locked:
#if OBOS_DEBUG
    lock->caller = __builtin_return_address(0);
#endif
    return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql)
{
#if OBOS_DEBUG
    if (oldIrql & 0xf0 && oldIrql != IRQL_INVALID)
    {
        OBOS_ASSERT(!"funny stuff");
        return OBOS_STATUS_INVALID_IRQL;
    }
#endif

    atomic_flag_clear_explicit(&lock->val, memory_order_relaxed);

#if OBOS_DEBUG
    lock->caller = 0;
#endif

    Core_LowerIrqlNoThread(oldIrql);

    return OBOS_STATUS_SUCCESS;
}
__attribute__((no_instrument_function)) OBOS_NO_UBSAN obos_status Core_SpinlockReleaseNoDPCDispatch(spinlock* const lock, irql oldIrql)
{
#if OBOS_DEBUG
    if (oldIrql & 0xf0 && oldIrql != IRQL_INVALID)
    {
        OBOS_ASSERT(!"funny stuff");
        return OBOS_STATUS_INVALID_IRQL;
    }
#endif

    atomic_flag_clear_explicit(&lock->val, memory_order_relaxed);

#if OBOS_DEBUG
    lock->caller = 0;
#endif

    Core_LowerIrqlNoDPCDispatch(oldIrql);

    return OBOS_STATUS_SUCCESS;
}

/*
 * No.
OBOS_NO_UBSAN void Core_SpinlockForcedRelease(spinlock* const lock)
{
    atomic_flag_clear_explicit(&lock->val, memory_order_seq_cst);
    lock->irqlNThrVariant = false;
#ifdef OBOS_DEBUG
    lock->owner = nullptr;
#endif
    lock->locked = false;
}*/

