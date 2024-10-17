/*
 * oboskrnl/scheduler/sched_sys.h
 *
 * Copyright (c) 2024 Omar Berrow
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
} thread_ctx_handle;

// scheduler/thread_context_info.h

handle Sys_ThreadContextCreate(uintptr_t entry, uintptr_t arg1, void* stack, size_t stack_size, handle vmm_context);
obos_status Sys_ThreadContextRead(handle thread_context, struct thread_context_info* out);

// scheduler/thread.h

handle Sys_ThreadOpen(handle proc, uint64_t tid);
handle Sys_ThreadCreate(thread_priority priority, thread_affinity affinity, handle thread_context);
obos_status Sys_ThreadReady(handle thread);
obos_status Sys_ThreadBlock(handle thread);
obos_status Sys_ThreadBoostPriority(handle thread, int reserved /* ignored as of now */);
obos_status Sys_ThreadPriority(handle thread, const thread_priority *new, thread_priority* old);
obos_status Sys_ThreadAffinity(handle thread, const thread_affinity *new, thread_affinity* old);
// Can only be called once per thread-object, and must be called before readying a thread.
obos_status Sys_ThreadSetOwner(handle thr, handle process);
uint64_t Sys_ThreadGetTid(handle thr);

// locks/wait.h

obos_status Sys_WaitOnObject(handle object /* must be a waitable handle */);
obos_status Sys_WaitOnObjects(handle *objects, size_t nObjects);

// scheduler/process.h

handle Sys_ProcessOpen(uint64_t pid);
handle Sys_ProcessStart(handle mainThread /* optional, set to HANDLE_INVALID if unwanted */, handle vmm_context);
obos_status Sys_ProcessKill(handle process, bool force);
