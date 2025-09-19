/*
 * drivers/generic/libps2/controller.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/event.h>

#include <vfs/keycode.h>

#if defined(__x86_64__) || defined(__i686__)
#   include <x86/i8042/ps2_irql.h>
#endif

enum {PS2_PORT_MAGIC=0x1BADBEEF};

enum {
    PS2_DEV_TYPE_UNKNOWN = 'u',
    PS2_DEV_TYPE_KEYBOARD = 'k',
    PS2_DEV_TYPE_MOUSE = 'm',
};

typedef struct ps2_port {
    union {
        uint64_t data;
        void* pdata;
    };
    union {
        uint64_t udata;
        void* pudata;
    };
    
    union {
        obos_status(*read_code)(void* handle, keycode* out, bool block);
        obos_status(*read_raw)(void* handle, void* buf, bool block);
    };
    // Gets the amount of readable objects for that handle.
    obos_status(*get_readable_count)(void* handle, size_t* nReadable);
    obos_status(*make_handle)(struct ps2_port* port, void** handle);
    // To be set by the driver when there is at least one object ready to be read by read_*
    // Should be a 'EVENT_NOTIFICATION'
    event* data_ready_event;

    // NOTE: Remove this and make it correct when the net-stack branch is
    // merged, and add the (un)reference_interface callbacks.
    void* default_handle;

    struct irq* irq;
    void(*data_ready)(struct ps2_port* channel, uint8_t data);
    
    size_t blk_size;

    union {
        OBOS_PACK struct {
            char id[4];
            uint8_t padding;
        };
        char str_id[5];
    };

    uint32_t magic;
    uint32_t gsi;
    
    uint16_t model;

    char type;

    bool works : 1;
    bool suppress_irqs : 1;
    bool second : 1;

    struct vnode* vn;
    struct dirent* ent;
} ps2_port;

DRV_EXPORT void PS2_DeviceWrite(bool channel_two, uint8_t val);
DRV_EXPORT uint8_t PS2_DeviceRead(uint32_t spin_timeout, obos_status* status);
DRV_EXPORT obos_status PS2_EnableChannel(bool channel_two, bool status);
DRV_EXPORT obos_status PS2_MaskChannelIRQs(bool channel_two, bool mask);
DRV_EXPORT obos_status PS2_FlushInput();
DRV_EXPORT ps2_port* PS2_GetPort(bool channel_two);