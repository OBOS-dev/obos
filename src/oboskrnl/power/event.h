/*
 * oboskrnl/power/event.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <locks/event.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <irq/dpc.h>

typedef struct power_event_header {
    event event;
    bool activated;
    const char* name;
    vnode* registered_to;
    dirent* dent;
    size_t trigger_count;
    dpc dpc;
} power_event_header;

enum {
    OBOS_POWER_BUTTON_EVENT,
    OBOS_POWER_MAX_VALUE = OBOS_POWER_BUTTON_EVENT,
};

extern power_event_header OBOS_PowerEvents[];

void OBOS_InitializeACPIEvents();