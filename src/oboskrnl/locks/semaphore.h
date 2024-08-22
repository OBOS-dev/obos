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
#include <locks/wait.h>

typedef struct semaphore {
    struct waitable_header hdr;
    spinlock lock;
    size_t count;
} semaphore;

#define SEMAPHORE_INITIALIZE(cnt) (semaphore){ .hdr=WAITABLE_HEADER_INITIALIZE(true, false), .count=cnt }

OBOS_EXPORT obos_status Core_SemaphoreAcquire(semaphore* sem);
OBOS_EXPORT obos_status Core_SemaphoreTryAcquire(semaphore* sem);
OBOS_EXPORT obos_status Core_SemaphoreRelease(semaphore* sem);
OBOS_EXPORT size_t Core_SemaphoreGetValue(semaphore* sem);