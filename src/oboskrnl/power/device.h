/*
 * oboskrnl/power/device.h
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * D state helpers.
 */

#pragma once

#include <int.h>
#include <error.h>

#include <uacpi/namespace.h>
#include <uacpi/sleep.h>

typedef enum d_state {
    DSTATE_INVALID = -1,
    DSTATE_0,
    DSTATE_1,
    DSTATE_2,
    DSTATE_3HOT,
    DSTATE_3COLD,
    DSTATE_MAX = DSTATE_3COLD,
} d_state;

// if dry_run is true, then the function does not actually put the device into the state,
// but only checks if it would be able to and returns an apprioriate status.

// TODO: PCI D State helpers?

OBOS_EXPORT obos_status OBOS_DeviceSetDState(uacpi_namespace_node* dev, d_state new_state, bool dry_run);
OBOS_EXPORT obos_status OBOS_DeviceHasDState(uacpi_namespace_node* dev, d_state state);

// Makes the device wake capable from sleep state 'state'
// state must be > S0 and < S5
OBOS_EXPORT obos_status OBOS_DeviceMakeWakeCapable(uacpi_namespace_node* dev, uacpi_sleep_state state, bool registerGPEOnly);

// Returns DSTATE_INVALID on error, or if the device does not need to be moved in another
// D state to wake us.
// Always check status to make sure.
OBOS_EXPORT d_state OBOS_DeviceGetDStateForWake(uacpi_namespace_node* dev, uacpi_sleep_state state, obos_status* status);
