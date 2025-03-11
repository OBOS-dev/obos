/*
 * drivers/generic/libps2/keyboard.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include "controller.h"

#define PS2K_ACK 0xfa
#define PS2K_RESEND 0xfe
#define PS2K_INVALID 0xff

enum {
    PS2K_MAGIC_VALUE = 0xFEE1DEAD
};
typedef struct keyboard_data {
    uint32_t ps2k_magic;
    ps2_port* port;
    uint8_t set;
    bool initialized : 1;
} keyboard_data;

void PS2_InitializeKeyboard(ps2_port* port);