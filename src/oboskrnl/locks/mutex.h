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

#include <locks/wait.h>

typedef struct mutex {
    struct waitable_header hdr;
    // Whether the mutex is locked or not.
    atomic_flag lock;
    bool locked;
    // set this when freeing an object.
    bool ignoreAllAndBlowUp;
    // The thread that took the mutex.
    thread* who;
} mutex;

#define MUTEX_INITIALIZE() (mutex){ .hdr=WAITABLE_HEADER_INITIALIZE(true, false), .locked=false, .who=nullptr }

obos_status Core_MutexAcquire(mutex* mut);
obos_status Core_MutexTryAcquire(mutex* mut);
obos_status Core_MutexRelease(mutex* mut);
bool Core_MutexAcquired(mutex* mut);