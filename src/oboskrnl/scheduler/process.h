/*
 * oboskrnl/scheduler/process.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

typedef struct process
{
	// If pid==1, this is the kernel process.
	uint64_t pid;
	thread_list threads;
	uid currentUID;
	gid currentGID;
	struct context* ctx;
} process;
extern uint64_t Core_NextPID;
// The first thread in this process must be the kernel main thread.
extern OBOS_EXPORT process* OBOS_KernelProcess;
/// <summary>
/// Allocates a process object.
/// </summary>
/// <param name="status">[out,opt] The status of the function.</param>
/// <returns>The object.</returns>
OBOS_EXPORT process* Core_ProcessAllocate(obos_status* status);
/// <summary>
/// Starts a process. Readies the mainThread passed.
/// </summary>
/// <param name="proc">A pointer to the process object.</param>
/// <param name="mainThread">The main thread of the process. Must be initialized, but not readied.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_ProcessStart(process* proc, thread* mainThread);
/// <summary>
/// Appends a thread to a process. Does not ready the thread passed to it.
/// </summary>
/// <param name="proc">The process.</param>
/// <param name="thread">The thread.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_ProcessAppendThread(process* proc, thread* thread);
/// <summary>
/// Terminates a process.
/// </summary>
/// <param name="proc">The process to terminate.</param>
/// <param name="forced">Whether to forcefully terminate the process.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_ProcessTerminate(process* proc, bool forced);