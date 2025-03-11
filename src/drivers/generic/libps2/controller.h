/*
 * drivers/generic/libps2/controller.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#if defined(__x86_64__) || defined(__i686__)
#   include <x86/i8042/ps2_irql.h>
#endif

typedef struct ps2_port {
    union {
        uint64_t data;
        void* pdata;
    };
    union {
        uint64_t udata;
        void* pudata;
    };
    struct irq* irq;
    void(*data_ready)(struct ps2_port* channel, uint8_t data);
    uint32_t gsi;
    bool works : 1;
    bool suppress_irqs : 1;
    bool second : 1;
} ps2_port;

void PS2_DeviceWrite(bool channel_two, uint8_t val);
uint8_t PS2_DeviceRead(uint32_t spin_timeout, obos_status* status);
obos_status PS2_EnableChannel(bool channel_two, bool status);
obos_status PS2_MaskChannelIRQs(bool channel_two, bool mask);
obos_status PS2_FlushInput();