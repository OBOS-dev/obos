/*
 * oboskrnl/scheduler/schedule.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
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

/// <summary>
/// Suspends or unsuspends the scheduler.<para/>
/// This causes it to hang indefinitely on yield. This is used so that we can save thread contexts<para/>
/// of all CPUs' threads before suspending the system.
/// </summary>
/// <param name="suspended">Whether to suspend (true) or unsuspend (false) the scheduler..</param>
OBOS_EXPORT void Core_SuspendScheduler(bool suspended);
/// <summary>
/// Waits for all CPUs (but the current CPU) to suspend their scheduler,
/// </summary>
OBOS_EXPORT void Core_WaitForSchedulerSuspend();

extern OBOS_EXPORT size_t Core_ReadyThreadCount;
extern struct irq* Core_SchedulerIRQ;
extern OBOS_EXPORT uint64_t Core_SchedulerTimerFrequency;
extern spinlock Core_SchedulerLock;
