/*
	oboskrnl/scheduler/schedule.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>

#include <locks/spinlock.h>

/// <summary>
/// Gets the current thread.
/// </summary>
/// <returns>The current thread.</returns>
OBOS_EXPORT thread* Core_GetCurrentThread();
/// <summary>
/// Schedules a thread.
/// Only provided for documentation purposes, never call directly without saving the current thread's context first
/// </summary>
void Core_Schedule();
/// <summary>
/// Yields the current thread. This will save the current thread context, then call Core_Schedule after raising the IRQL (if needed).
/// </summary>
OBOS_EXPORT void Core_Yield();

extern OBOS_EXPORT size_t Core_ReadyThreadCount;
extern struct irq* Core_SchedulerIRQ;
extern OBOS_EXPORT uint64_t Core_SchedulerTimerFrequency;
extern spinlock Core_SchedulerLock;