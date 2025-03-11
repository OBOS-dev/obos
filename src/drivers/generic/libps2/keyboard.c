/*
 * drivers/generic/libps2/keyboard.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <stdarg.h>

#include <irq/irql.h>

#include "keyboard.h"
#include "controller.h"

static void keyboard_ready(ps2_port* port, uint8_t scancode)
{
    if (!((keyboard_data*)port->pudata)->initialized)
        return;
    (void)scancode;
}

static OBOS_PAGEABLE_FUNCTION uint8_t send_command_impl(ps2_port* port, obos_status* status, uint8_t cmd, size_t nArgs, va_list list)
{
    PS2_DeviceWrite(port->second, cmd);
    for (size_t i = 0; i < nArgs; i++)
        PS2_DeviceWrite(port->second, va_arg(list, uint32_t) & 0xff);
    return PS2_DeviceRead(0xffff, status);
}

struct keyboard_data keyboard_data_buf[2];

static OBOS_PAGEABLE_FUNCTION uint8_t send_command(ps2_port* port, uint8_t cmd, size_t nArgs, ...)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    va_list list;
    va_start(list, nArgs);
    uint8_t res = send_command_impl(port, &status, cmd, nArgs, list);
    if (obos_is_error(status))
    {
        OBOS_Warning("Timeout while waiting for a response from the PS/2 Keyboard. Aborting\n");
        res = PS2K_INVALID;
        goto done;
    }
    if (res == PS2K_ACK)
        goto ack;
    ack:
    done:
    va_end(list);
    return res;
}

OBOS_PAGEABLE_FUNCTION void PS2_InitializeKeyboard(ps2_port* port)
{
    OBOS_ASSERT(Core_GetIrql() < IRQL_DISPATCH);
    OBOS_Log("PS/2: Initializing PS/2 Keyboard on channel %c\n", port->second ? '2' : '1');
    // PS2_MaskChannelIRQs(port->second, true);
    irql oldIrql = Core_RaiseIrql(IRQL_PS2);

    // port->suppress_irqs = true;

    port->pudata = &keyboard_data_buf[port->second];
    keyboard_data* data = port->pudata;
    data->ps2k_magic = PS2K_MAGIC_VALUE;
    data->port = port;
    data->initialized = false;

    port->data_ready = keyboard_ready;

    uint8_t res = send_command(port, 0xff, 0);
    if (res != 0xfa)
    {
        Core_LowerIrql(oldIrql);
        return;
    }
    res = PS2_DeviceRead(1024, nullptr);
    if (res != 0xAA)
    {
        OBOS_Warning("PS/2: While resetting PS/2 keyboard: Got 0x%02x instead of 0xaa (test success code). Aborting initialization\n", res);
        Core_LowerIrql(oldIrql);
        return;
    }

	// Keys need to held for 250 ms before repeating, and they repeat at a rate of 30 hz (33.33333 ms).
    res = send_command(port, 0xf3, 1, 0x00);
    if (res != PS2K_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }

    // Enable scanning.
    res = send_command(port, 0xf4, 0);
    if (res != PS2K_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }

    // Clear keyboard LEDs.
    res = send_command(port, 0xed, 1, 0x0);
    if (res != PS2K_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }
    
    Core_LowerIrql(oldIrql);

    PS2_FlushInput();

    port->suppress_irqs = true;

    uint8_t set = 0;
    // Try putting the keyboard into scancode set #2 by default, if that doesn't work (it sends RESEND), put it into scancode set #1
    set = 2;
    bool found = false;
    while (!found)
    {
        bool retried = false;
        res = send_command(port, 0xf0, 1, set);
        if (res == PS2K_RESEND && !retried)
        {
            retried = true;
            continue;
        }
        if (set == 1 && res == PS2K_RESEND)
        {
            set = 0;
            break;
        }
        if (res == PS2K_RESEND)
            set = 1;
        else
            found = true;
    }
    if (!set)
    {
        OBOS_Error("PS/2: Could not put the keyboard into a defined scancode set (tried sets one and two, neither were recognized).\n");
        return;
    }

    port->suppress_irqs = false;

    data->set = set;
    OBOS_Log("PS/2: Successfully initialized keyboard on channel %c\n", port->second ? '2' : '1');
    OBOS_Debug("PS/2 Keyboard is using scancode set %d\n", data->set);
    data->initialized = true;
}