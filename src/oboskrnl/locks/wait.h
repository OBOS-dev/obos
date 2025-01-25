/*
 * oboskrnl/locks/wait.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

#include <locks/spinlock.h>

struct waitable_header
{
    thread_list waiting;
    spinlock lock;
    bool signaled : 1;
    bool use_signaled : 1;
    bool interrupted : 1;
};

#define WAITABLE_HEADER_INITIALIZE(s, use) (struct waitable_header){ .waiting={}, .signaled=(s), .use_signaled=(use) }

// obj must be a type with `struct waitable_header` as its first member
// otherwise, this will not work, and will corrupt stuff.
#define WAITABLE_OBJECT(obj) (struct waitable_header*)(&(obj))

OBOS_EXPORT obos_status Core_WaitOnObject(struct waitable_header* obj);
OBOS_EXPORT obos_status CoreH_SignalWaitingThreads(struct waitable_header* obj, bool all, bool boostPriority);
OBOS_EXPORT void        CoreH_ClearSignaledState(struct waitable_header* obj);
OBOS_EXPORT obos_status CoreH_AbortWaitingThreads(struct waitable_header* obj);
