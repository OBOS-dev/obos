/*
 * oboskrnl/locks/semaphore.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

#include <stdatomic.h>

#include <locks/spinlock.h>

typedef struct semaphore {
    spinlock lock;
    size_t count;
    // The threads waiting for the mutex to be released.
    thread_list waiting;
} semaphore;

#define SEMAPHORE_INITIALIZE(cnt) { .count=cnt, .waiting={} }

obos_status Core_SemaphoreAcquire(semaphore* sem);
obos_status Core_SemaphoreTryAcquire(semaphore* sem);
obos_status Core_SemaphoreRelease(semaphore* sem);
size_t Core_SemaphoreGetValue(semaphore* sem);