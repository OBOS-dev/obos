/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdatomic.h>

#include <irq/irql.h>

typedef struct {
	atomic_flag val;
	bool locked;
	bool irqlNThrVariant; // Value of irqlNthrVariant
#ifdef OBOS_DEBUG
	struct thread* owner; // for debugging purposes only
#endif
} spinlock;

OBOS_EXPORT spinlock Core_SpinlockCreate();
OBOS_EXPORT irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant);
OBOS_EXPORT irql Core_SpinlockAcquire(spinlock* const lock);
OBOS_EXPORT obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
OBOS_EXPORT void Core_SpinlockForcedRelease(spinlock* const lock);
OBOS_EXPORT bool Core_SpinlockAcquired(spinlock* const lock);