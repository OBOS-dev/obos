/*
 * drivers/generic/libps2/mouse.h
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
#include "ringbuffer.h"

enum {
    PS2M_YOF = BIT(7),
    PS2M_XOF = BIT(6),
    PS2M_YS = BIT(5),
    PS2M_XS = BIT(4),
    PS2M_BM = BIT(2),
    PS2M_BR = BIT(1),
    PS2M_BL = BIT(0),
};

typedef struct ps2m_basic_pckt {
    uint8_t flags;
    uint8_t x;
    uint8_t y;
} ps2m_basic_pckt;

typedef struct ps2m_z_exten_pckt {
    uint8_t flags;
    uint8_t x;
    uint8_t y;
    int8_t z;
} ps2m_z_exten_pckt;

enum {
    PS2M_FLAGS2_Z_MASK = 0xf,
    PS2M_FLAGS2_B4 = BIT(4),
    PS2M_FLAGS2_B5 = BIT(5),
};

typedef struct ps2m_b4b5_exten_pckt {
    uint8_t flags;
    uint8_t x;
    uint8_t y;
    uint8_t flags2;
} ps2m_b4b5_exten_pckt;

enum {
    PS2M_MAGIC_VALUE = 0xBEEDDEAD, // BEAD DEAD?
    PS2M_HND_MAGIC_VALUE = 0xBADDA600 // BAD DAY? (TIME??) ((SANS REFERENCE??))
};

typedef struct ps2m_data {
    uint32_t magic; // PS2M_MAGIC_VALUE

    ps2_port* port;
    ps2_ringbuffer packets;
    dpc dpc;

    bool initialized : 1;
    bool z_axis_extension_enabled : 1;
    bool b4b5_extension_enabled : 1;

    union {
        struct ps2m_basic_pckt basic_pckt;
        struct ps2m_b4b5_exten_pckt b5_pckt;
        struct ps2m_z_exten_pckt z_pckt;
        uint8_t raw_pckt[4];
    };
    uint8_t nReady;
} ps2m_data;
#define ps2m_enough_data(m) ((m)->nReady >= ((m)->z_axis_extension_enabled ? 4 : 3))

typedef struct ps2m_handle {
    uint32_t magic; // PS2M_HND_MAGIC_VALUE
    ps2_port* port;
    size_t in_ptr;
} ps2m_handle;

void PS2_InitializeMouse(ps2_port* port);
void PS2_StartMouse(ps2_port* port);
void PS2_FreeMouse(ps2_port* port);