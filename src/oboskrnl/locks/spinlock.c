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
	if (Core_GetIrql() < minIrql)
		oldIrql = Core_RaiseIrqlNoThread(minIrql);
	if (!atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
       goto done_wait;
    while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
		OBOSS_SpinlockHint();
    done_wait:
	lock->locked = true;
	return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN irql Core_SpinlockAcquire(spinlock* const lock)
{
    irql oldIrql = IRQL_INVALID;
    if (Core_GetIrql() < IRQL_DISPATCH)
        oldIrql = Core_RaiseIrqlNoThread(IRQL_DISPATCH);
    if (!atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
       goto locked;
	while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
		OBOSS_SpinlockHint();
    locked:
	lock->locked = true;
#if OBOS_ENABLE_LOCK_PROFILING
	lock->lastLockTimeNS = nanoseconds_since_boot();
#endif
	return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql)
{
	if (oldIrql & 0xf0 && oldIrql != IRQL_INVALID)
	{
		OBOS_ASSERT(!"funny stuff");
		return OBOS_STATUS_INVALID_IRQL;
	}
	atomic_flag_clear_explicit(&lock->val, memory_order_relaxed);
	lock->locked = false;
	Core_LowerIrqlNoThread(oldIrql);
#if OBOS_ENABLE_LOCK_PROFILING
	lock->lastLockTimeNS = nanoseconds_since_boot() - lock->lastLockTimeNS;
#endif
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

__attribute__((no_instrument_function)) bool Core_SpinlockAcquired(spinlock* const lock)
{
	return lock ? lock->locked : false;
}
