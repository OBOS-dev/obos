/*
	oboskrnl/locks/spinlock.c

	Copyright (c) 2024 Omar Berrow
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

spinlock Core_SpinlockCreate()
{
	spinlock tmp = {};
	tmp.val = (atomic_flag)ATOMIC_FLAG_INIT;
	return tmp;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant)
{
	if (!lock)
		return IRQL_INVALID;
	if (minIrql & 0xf0 && minIrql != IRQL_INVALID)
		return IRQL_INVALID;
	// IRQL_INVALID is used to specify not to raise the irql at all
	// this way, LowerIrql can lock the DPC queue without breaking.
#ifdef OBOS_DEBUG
	if (lock->owner == Core_GetCurrentThread() && lock->owner)
		OBOS_Warning("Recursive lock taken!\n");
	// if (++lock->nCPUsWaiting == Core_CpuCount && Core_CpuCount != 1)
	// 	OBOS_Warning("Deadlocked! Thread with spinlock has a tid of %ld.\n", lock->owner ? lock->owner->tid : UINT64_MAX);
#endif
	irql oldIrql = IRQL_INVALID;
	if (Core_GetIrql() < minIrql)
		oldIrql = irqlNthrVariant ?
			Core_RaiseIrqlNoThread(minIrql) :
			Core_RaiseIrql(minIrql);
	while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
		OBOSS_SpinlockHint();
	// lock->nCPUsWaiting--;
	lock->irqlNThrVariant = irqlNthrVariant;
#ifdef OBOS_DEBUG
	lock->owner = Core_GetCurrentThread();
#endif
	lock->locked = true;
	lock->lastLockTimeNS = nanoseconds_since_boot();
	return oldIrql;
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN irql Core_SpinlockAcquire(spinlock* const lock)
{
	irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
	while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_acq_rel))
		OBOSS_SpinlockHint();
	lock->irqlNThrVariant = false;
#ifdef OBOS_DEBUG
	lock->owner = Core_GetCurrentThread();
#endif
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
#ifdef OBOS_DEBUG
	lock->owner = nullptr;
#endif
	lock->locked = false;
	if (oldIrql != IRQL_INVALID)
		lock->irqlNThrVariant ? Core_LowerIrqlNoThread(oldIrql) : Core_LowerIrql(oldIrql);
	lock->irqlNThrVariant = false;
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
