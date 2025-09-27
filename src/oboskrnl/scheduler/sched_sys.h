/*
 * oboskrnl/scheduler/sched_sys.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>

#include <scheduler/thread.h>

#include <locks/pushlock.h>

// internal
typedef struct thread_ctx_handle
{
    // NOTE: When the thread context is finally used in a thread creation, ctx gets freed,
    // canFree is set to false, then ctx is set to &thread->context.
    struct thread_context_info* ctx;
    pushlock lock;
    // If false, this thread context cannot be used in a new thread creation.
    bool canFree;

    struct context* vmm_ctx;
} thread_ctx_handle;

// scheduler/thread_context_info.h

handle Sys_ThreadContextCreate(uintptr_t entry, uintptr_t arg1, void* stack, size_t stack_size, handle vmm_context);

// scheduler/thread.h

handle Sys_ThreadOpen(handle proc, uintptr_t tid);
handle Sys_ThreadCreate(thread_priority priority, thread_affinity affinity, handle thread_context);
obos_status Sys_ThreadReady(handle thread);
obos_status Sys_ThreadBlock(handle thread);
obos_status Sys_ThreadBoostPriority(handle thread, int reserved /* ignored as of now */);
obos_status Sys_ThreadPriority(handle thread, const thread_priority *new, thread_priority* old);
obos_status Sys_ThreadAffinity(handle thread, const thread_affinity *new, thread_affinity* old);
// Can only be called once per thread-object, and must be called before readying a thread.
obos_status Sys_ThreadSetOwner(handle thr, handle process);
uintptr_t Sys_ThreadGetTid(handle thr);

// locks/wait.h

obos_status Sys_WaitOnObject(handle object /* must be a waitable handle */);

// scheduler/process.h

handle Sys_ProcessOpen(uintptr_t pid);
handle Sys_ProcessStart(handle mainThread /* optional, set to HANDLE_INVALID if unwanted */, handle vmm_context, bool is_fork);
uint32_t Sys_ProcessGetStatus(handle process);
uintptr_t Sys_ProcessGetPID(handle process);
uintptr_t Sys_ProcessGetPPID(handle process);
// Gets a handle to any arbitrary child process.
handle Sys_ProcessGetChildHandle();

#define WNOHANG 1
#define WSTOPPED 2
#define WEXITED 4
#define WCONTINUED 8

// If HANDLE_ANY to make it choose an arbitrary child process.
obos_status Sys_WaitProcess(handle proc, int* status, int options, uint32_t* ret_pid);

obos_status Sys_SetUid(uid to);
obos_status Sys_SetGid(gid to);
uid Sys_GetUid();
gid Sys_GetGid();
