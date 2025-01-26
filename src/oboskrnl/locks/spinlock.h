/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdatomic.h>

#include <irq/irql.h>

typedef struct spinlock {
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) atomic_flag val;
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) bool locked;
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) bool irqlNThrVariant; // Value of irqlNthrVariant
#ifdef OBOS_DEBUG
	// for debugging purposes only
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) struct thread* owner;
#else
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) uintptr_t resv1;
#endif
	// The last lock time, in nanoseconds.
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) uint64_t lastLockTimeNS;
} spinlock;

OBOS_EXPORT spinlock Core_SpinlockCreate();
OBOS_EXPORT irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant);
OBOS_EXPORT irql Core_SpinlockAcquire(spinlock* const lock);
OBOS_EXPORT obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
OBOS_EXPORT bool Core_SpinlockAcquired(spinlock* const lock);
