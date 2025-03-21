/*
 * drivers/generic/libps2/keyboard.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/keycode.h>

#include <locks/event.h>

#include <irq/dpc.h>

#include "controller.h"

#define PS2_ACK 0xfa
#define PS2_RESEND 0xfe
#define PS2_INVALID_RESPONSE 0xff

enum {
    PS2K_MAGIC_VALUE = 0xFEE1DEAD,
    PS2K_HND_MAGIC_VALUE = 0xFEE1DEAE
};

typedef struct ps2k_ringbuffer
{
    event e;
    union {
        void* buff;
        keycode* keycodes;
    };
    size_t size;
    size_t nElements;
    size_t out_ptr;
    size_t handle_count;
} ps2k_ringbuffer;

typedef struct ps2k_handle {
    uint32_t magic; // PS2K_HND_MAGIC_VALUE
    ps2_port* port;
    size_t in_ptr;
} ps2k_handle;

obos_status PS2_RingbufferInitialize(ps2k_ringbuffer* buff);
obos_status PS2_RingbufferAppend(ps2k_ringbuffer* buff, keycode code, bool signal);
obos_status PS2_RingbufferFetch(const ps2k_ringbuffer* buff, size_t* in_ptr, keycode* code);
obos_status PS2_RingbufferFree(ps2k_ringbuffer* buff);

typedef struct ps2k_data {
    struct ps2k_ringbuffer input;
    ps2_port* port;
    uint32_t ps2k_magic;
    dpc dpc;
    uint8_t set;
    bool initialized : 1;
    bool processing_extended : 1;
    bool processing_release : 1; // only valid if set == 2
    bool super_key : 1;
    bool caps_lock : 1;
    bool num_lock : 1;
    bool ctrl : 1;
    bool shift : 1;
    bool alt : 1;
    bool fn : 1;
} ps2k_data;

void PS2_InitializeKeyboard(ps2_port* port);
void PS2_FreeKeyboard(ps2_port* port);