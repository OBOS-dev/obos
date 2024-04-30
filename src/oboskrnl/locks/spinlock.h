/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <irq/irql.h>

typedef bool spinlock;

spinlock Core_SpinlockCreate();
irql Core_SpinlockAcquireExplicit(spinlock* const lock, irql minIrql);
irql Core_SpinlockAcquire(spinlock* const lock);
void Core_SpinlockRelease(spinlock* const lock, irql oldIrql);
void Core_SpinlockForcedRelease(spinlock* const lock);
bool Core_SpinlockIsAcquired(const spinlock* const lock);