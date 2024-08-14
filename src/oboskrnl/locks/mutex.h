/*
 * oboskrnl/locks/mutex.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

#include <stdatomic.h>

typedef struct mutex {
    // Whether the mutex is locked or not.
    atomic_flag lock;
    bool locked;
    // The threads waiting for the mutex to be released.
    thread_list waiting;
    // The thread that took the mutex.
    thread* who;
} mutex;

#define MUTEX_INITIALIZE() (mutex){ .locked=false, .waiting={}, .who=nullptr }

obos_status Core_MutexAcquire(mutex* mut);
obos_status Core_MutexTryAcquire(mutex* mut);
obos_status Core_MutexRelease(mutex* mut);
bool Core_MutexAcquired(mutex* mut);