/*
	oboskrnl/locks/spinlock.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <stdatomic.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/schedule.h>

#ifdef __x86_64__
#	define spinlock_hint() __builtin_ia32_pause()
#endif

spinlock Core_SpinlockCreate()
{
	spinlock tmp = ATOMIC_FLAG_INIT;
	return tmp;
}
OBOS_NO_UBSAN irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant)
{
	if (!lock)
		return IRQL_INVALID;
	if (minIrql & 0xf0)
		return IRQL_INVALID;
#ifdef OBOS_DEBUG
	if (lock->owner == Core_GetCurrentThread() && lock->owner)
		OBOS_Warning("Recursive lock taken!\n");
#endif
	irql newIrql = Core_GetIrql() < minIrql ? irqlNthrVariant ? Core_RaiseIrqlNoThread(minIrql) : Core_RaiseIrql(minIrql) : IRQL_INVALID;
	while (atomic_flag_test_and_set_explicit(&lock->val, memory_order_seq_cst))
		spinlock_hint();
	lock->irqlNThrVariant = irqlNthrVariant;
#ifdef OBOS_DEBUG
	lock->owner = Core_GetCurrentThread();
#endif
	return newIrql;
}
irql Core_SpinlockAcquire(spinlock* const lock)
{
	return Core_SpinlockAcquireExplicit(lock, IRQL_MASKED, false);
}
OBOS_NO_UBSAN obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql)
{
	if (oldIrql & 0xf0 && oldIrql != IRQL_INVALID)
		return OBOS_STATUS_INVALID_IRQL;
	atomic_flag_clear_explicit(&lock->val, memory_order_seq_cst);
#ifdef OBOS_DEBUG
	lock->owner = nullptr;
#endif
	if (oldIrql != IRQL_INVALID)
		lock->irqlNThrVariant ? Core_LowerIrqlNoThread(oldIrql) : Core_LowerIrql(oldIrql);
	lock->irqlNThrVariant = false;
	return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN void Core_SpinlockForcedRelease(spinlock* const lock)
{
	atomic_flag_clear_explicit(&lock->val, memory_order_seq_cst);
	lock->irqlNThrVariant = false;
#ifdef OBOS_DEBUG
	lock->owner = nullptr;
#endif
}