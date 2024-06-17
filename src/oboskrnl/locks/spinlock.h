/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdatomic.h>

#include <irq/irql.h>

typedef atomic_flag spinlock;

spinlock Core_SpinlockCreate();
irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql);
irql Core_SpinlockAcquire(spinlock* const lock);
obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
void Core_SpinlockForcedRelease(spinlock* const lock);