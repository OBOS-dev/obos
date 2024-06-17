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

#ifdef __x86_64__
#	define spinlock_hint() __builtin_ia32_pause()
#endif

spinlock Core_SpinlockCreate()
{
	spinlock tmp = ATOMIC_FLAG_INIT;
	return tmp;
}
OBOS_NO_UBSAN irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql)
{
	if (!lock)
		return IRQL_INVALID;
	if (minIrql & 0xf0)
		return IRQL_INVALID;
	const bool expected = false;
	irql newIrql = Core_GetIrql() < minIrql ? Core_RaiseIrql(minIrql) : IRQL_INVALID;
	while (atomic_flag_test_and_set_explicit(lock, memory_order_seq_cst))
		spinlock_hint();
	return newIrql;
}
irql Core_SpinlockAcquire(spinlock* const lock)
{
	return Core_SpinlockAcquireExplicit(lock, IRQL_MASKED);
}
OBOS_NO_UBSAN obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql)
{
	if (oldIrql & 0xf0 && oldIrql != IRQL_INVALID)
		return OBOS_STATUS_INVALID_IRQL;
	atomic_flag_clear_explicit(lock, memory_order_seq_cst);
	if (oldIrql != IRQL_INVALID)
		Core_LowerIrql(oldIrql);
	return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN void Core_SpinlockForcedRelease(spinlock* const lock)
{
	atomic_flag_clear_explicit(lock, memory_order_seq_cst);
}