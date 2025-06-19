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
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) atomic_flag val;
	OBOS_ALIGNAS(OBOS_ARCHITECTURE_BITS/8) bool locked;
} spinlock;

#define Core_SpinlockCreate() (spinlock){}
OBOS_EXPORT irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql, bool irqlNthrVariant);
OBOS_EXPORT irql Core_SpinlockAcquire(spinlock* const lock);
OBOS_EXPORT obos_status Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
OBOS_EXPORT bool Core_SpinlockAcquired(spinlock* const lock);
