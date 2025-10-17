/*
 * drivers/generic/libps2/ringbuffer.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/keycode.h>
#include <vfs/mouse.h>

#include <locks/event.h>

typedef struct ps2_ringbuffer
{
    event e;
    union {
        void* buff;
        keycode* keycodes;
        mouse_packet* mouse_packets;
    };
    size_t size;
    size_t nElements;
    size_t out_ptr;
    size_t handle_count;
} ps2_ringbuffer;

obos_status PS2_RingbufferInitialize(ps2_ringbuffer* buff, bool mouse);

obos_status PS2_RingbufferAppendKeycode(ps2_ringbuffer* buff, keycode code, bool signal);
obos_status PS2_RingbufferFetchKeycode(const ps2_ringbuffer* buff, size_t* in_ptr, keycode* code);

obos_status PS2_RingbufferAppendMousePacket(ps2_ringbuffer* buff, mouse_packet pckt, bool signal);
obos_status PS2_RingbufferFetchMousePacket(const ps2_ringbuffer* buff, size_t* in_ptr, mouse_packet* pckt);

obos_status PS2_RingbufferFree(ps2_ringbuffer* buff);