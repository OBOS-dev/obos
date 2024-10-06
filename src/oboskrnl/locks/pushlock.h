/*
 * oboskrnk/locks/pushlock.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/wait.h>

#include <scheduler/thread.h>

typedef struct pushlock
{
    struct waitable_header hdr; // for writers
    _Atomic(size_t) nReaders; // if > 0, wait on hdr, when it gets to zero, signal hdr.
    thread* currWriter; // if == nullptr, no one is writing
} pushlock;

#define PUSHLOCK_INITIALIZE() (pushlock){ .hdr=WAITABLE_HEADER_INITIALIZE(true, true), .nReaders=0, .currWriter=nullptr }

OBOS_EXPORT obos_status Core_PushlockAcquire(pushlock* lock, bool reader /* false: writer, true: reader */);
OBOS_EXPORT obos_status Core_PushlockTryAcquire(pushlock* lock); // Only for writers.
OBOS_EXPORT obos_status Core_PushlockRelease(pushlock* lock, bool reader);
OBOS_EXPORT size_t Core_PushlockGetReaderCount(pushlock* lock);
OBOS_EXPORT thread* Core_PushlockGetWriter(const pushlock* lock);