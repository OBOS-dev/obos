/*
 * drivers/x86/uart/ps2_structs.h
 * 
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/spinlock.h>

#include <irq/irq.h>
#include <irq/irql.h>

#define PS2_DATA 0x60
#define PS2_CMD_STATUS 0x64

enum {
    PS2_OUTPUT_BUFFER_FULL = BIT(0),
    PS2_INPUT_BUFFER_FULL = BIT(1),
    PS2_SYSTEM_FLAG = BIT(2),
    // "0 = data written to input buffer is data for PS/2 device, 1 = data written to input buffer is data for PS/2 controller command"
    PS2_CMD_DATA = BIT(3), 
    PS2_TIMEOUT = BIT(6),
    PS2_PARITY_ERROR = BIT(7),
};

#define PS2_CTLR_READ_RAM_CMD(n) (0x20+((n)&0x1f))
#define PS2_CTLR_WRITE_RAM_CMD(n) (0x60+((n)&0x1f))
#define PS2_CTLR_DISABLE_PORT_TWO 0xA7
#define PS2_CTLR_ENABLE_PORT_TWO 0xA8
#define PS2_CTLR_TEST_PORT_TWO 0xA9
#define PS2_CTLR_TEST 0xAA
#define PS2_CTLR_TEST_PORT_ONE 0xAB
#define PS2_CTLR_DUMP_RAM 0xAC
#define PS2_CTLR_DISABLE_PORT_ONE 0xAD
#define PS2_CTLR_ENABLE_PORT_ONE 0xAE
#define PS2_CTLR_READ_CTLR_OUT_BUFFER 0xD0
#define PS2_CTLR_WRITE_CTLR_OUT_BUFFER 0xD1
#define PS2_CTLR_WRITE_PORT_TWO 0xD4

enum {
    PS2_CTLR_CONFIG_PORT_ONE_IRQ = BIT(0),
    PS2_CTLR_CONFIG_PORT_TWO_IRQ = BIT(1),
    PS2_CTLR_CONFIG_SYSTEM_FLAG = BIT(2),
    PS2_CTLR_CONFIG_PORT_ONE_CLOCK = BIT(4),
    PS2_CTLR_CONFIG_PORT_TWO_CLOCK = BIT(5),
    PS2_CTLR_CONFIG_PORT_ONE_TRANSLATION = BIT(6),
};

#define IRQL_PS2 3

typedef struct ps2_port {
    uint32_t gsi;
    irq* irq;
    void(*data_ready)(uint8_t data);
    bool works : 1;
    bool second : 1;
} ps2_port;

extern struct ps2_ctlr_data {
    bool dual_channel : 1;
    ps2_port ports[2];
    spinlock lock;
} PS2_CtlrData;

obos_status PS2_InitializeController();
void PS2_DeviceWrite(bool port_two, uint8_t val);
uint8_t PS2_DeviceRead(uint32_t spin_timeout, obos_status* status);