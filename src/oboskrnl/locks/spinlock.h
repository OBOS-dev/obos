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
	// for debugging purposes only
	struct thread* owner;
	// size_t nCPUsWaiting; // If == Core_CpuCount, you have a deadlock on your hands.
#else
	uintptr_t resv1;
	size_t resv2;
#endif
} spinlock;

OBOS_EXPORT spinlock Core_SpinlockCreate();
OBOS_EXPORT irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant);
OBOS_EXPORT irql Core_SpinlockAcquire(spinlock* const lock);
OBOS_EXPORT obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
OBOS_EXPORT bool Core_SpinlockAcquired(spinlock* const lock);
