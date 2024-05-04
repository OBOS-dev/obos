/*
	oboskrnl/scheduler/schedule.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>

/// <summary>
/// Gets the current thread.
/// </summary>
/// <returns>The current thread.</returns>
thread* Core_GetCurrentThread();
/// <summary>
/// Schedules a thread.
/// Only provided for documentation purposes, never call directly without saving the current thread's context first
/// </summary>
void Core_Schedule();
/// <summary>
/// Yields the current thread. This will save the current thread context, then call Core_Schedule after raising the IRQL (if needed).
/// </summary>
void Core_Yield();

extern size_t Core_ReadyThreadCount;