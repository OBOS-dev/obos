/*
	oboskrnl/locks/spinlock.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <stdatomic.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#ifdef __x86_64__
#	define spinlock_hint() __asm__ volatile ("pause")
#endif

spinlock Core_SpinlockCreate()
{
	return false;
}
irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql)
{
	if (!lock)
		return IRQL_INVALID;
	if (minIrql & 0xf0)
		return IRQL_INVALID;
	const bool expected = true;
	irql newIrql = Core_GetIrql() < minIrql ? Core_RaiseIrql(minIrql) : IRQL_INVALID;
	while (atomic_compare_exchange_strong(lock, &expected, false))
		spinlock_hint();
	return newIrql;
}
irql Core_SpinlockAcquire(spinlock* const lock)
{
	return Core_SpinlockAcquireExplicit(lock, IRQL_MASKED);
}
obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql)
{
	if (oldIrql & 0xf0)
		return OBOS_STATUS_INVALID_IRQL;
	atomic_store(lock, false);
	if (oldIrql != IRQL_INVALID)
		Core_LowerIrql(oldIrql);
}
void Core_SpinlockForcedRelease(spinlock* const lock)
{
	atomic_store(lock, false);
}
bool Core_SpinlockIsAcquired(const spinlock* const lock)
{
	return atomic_load(lock);
}