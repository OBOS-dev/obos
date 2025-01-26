/*
 * oboskrnk/locks/rw_lock.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/wait.h>

#include <scheduler/thread.h>

typedef struct rw_lock
{
    struct waitable_header hdr; // for writers
    _Atomic(size_t) nReaders; // if > 0, wait on hdr (if writer), when it gets to zero, signal hdr.
    _Atomic(size_t) nWaitingReaders;
    thread* currWriter; // if == nullptr, no one is writing
    bool abort;
} rw_lock;

#define RWLOCK_INITIALIZE() (rw_lock){ .hdr=WAITABLE_HEADER_INITIALIZE(true, true), .nReaders=0, .currWriter=nullptr }

OBOS_EXPORT obos_status Core_RwLockAcquire(rw_lock* lock, bool reader /* false: writer, true: reader */);
OBOS_EXPORT obos_status Core_RwLockTryAcquire(rw_lock* lock); // Only for writers.
OBOS_EXPORT obos_status Core_RwLockRelease(rw_lock* lock, bool reader);
OBOS_EXPORT size_t Core_RwLockGetReaderCount(rw_lock* lock);
OBOS_EXPORT thread* Core_RwLockGetWriter(const rw_lock* lock);
