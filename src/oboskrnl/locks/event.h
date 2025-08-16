/*
 * oboskrnl/locks/event.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>

#include <stdatomic.h>

#include <locks/wait.h>

typedef enum event_type
{
    EVENT_NOTIFICATION,
    EVENT_SYNC,
} event_type;
typedef volatile struct event {
    struct waitable_header hdr;
    event_type type;
} event;

#define EVENT_INITIALIZE(t) (event){ .hdr=WAITABLE_HEADER_INITIALIZE(false, true), .type=(t) }

OBOS_EXPORT obos_status Core_EventPulse(event* event, bool boostWaitingThreadPriority);
OBOS_EXPORT bool        Core_EventGetState(const event* event);
OBOS_EXPORT obos_status Core_EventReset(event* event);
OBOS_EXPORT obos_status Core_EventSet(event* event, bool boostWaitingThreadPriority);
OBOS_EXPORT obos_status Core_EventClear(event* event);