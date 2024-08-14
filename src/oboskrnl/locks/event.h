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

typedef enum event_type
{
    EVENT_NOTIFICATION,
    EVENT_SYNC,
} event_type;
typedef struct event {
    atomic_bool signaled;
    thread_list waiting;
    event_type type;
} event;

#define EVENT_INITIALIZE(t) (event){ .signaled=0, .waiting={}, .type=(t) }

obos_status Core_EventPulse(event* event, bool boostWaitingThreadPriority);
bool        Core_EventGetState(const event* event);
obos_status Core_EventReset(event* event);
obos_status Core_EventSet(event* event, bool boostWaitingThreadPriority);
obos_status Core_EventClear(event* event);
obos_status Core_EventWait(event* event);