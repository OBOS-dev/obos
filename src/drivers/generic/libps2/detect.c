/*
 * generic/libps2/detect.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <stdarg.h>

#include "controller.h"
#include "detect.h"
#include "keyboard.h"

static OBOS_PAGEABLE_FUNCTION uint8_t send_command_impl(ps2_port* port, obos_status* status, uint8_t cmd, size_t nArgs, va_list list)
{
    PS2_DeviceWrite(port->second, cmd);
    for (size_t i = 0; i < nArgs; i++)
        PS2_DeviceWrite(port->second, va_arg(list, uint32_t) & 0xff);
    return PS2_DeviceRead(0x20000, status);
}

OBOS_PAGEABLE_FUNCTION uint8_t PS2_SendCommand(ps2_port* port, uint8_t cmd, size_t nArgs, ...)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    va_list list;
    va_start(list, nArgs);
    uint8_t res = send_command_impl(port, &status, cmd, nArgs, list);
    if (obos_is_error(status))
    {
        OBOS_Warning("Timeout while waiting for a response from the PS/2 Device on channel %c. Aborting\n", port->second ? '2' : '1');
        res = PS2_INVALID_RESPONSE;
        goto done;
    }
    if (res == PS2_ACK)
        goto ack;
    ack:
    done:
    va_end(list);
    return res;
}

void identify_device(ps2_port* port, uint16_t* const model, uint8_t* const type)
{
    PS2_SendCommand(port, 0xf2, 0);
    uint16_t byte_one = PS2_DeviceRead(0x20000, nullptr);
    uint16_t byte_two = PS2_DeviceRead(0x20000, nullptr);
    if ((byte_one == 0xff) && (byte_two == 0xff))
    {
        // An old device, assume it's a keyboard.
        *type = PS2_DEV_TYPE_KEYBOARD;
        goto done;    
    }
    if (byte_two == 0xff)
        byte_two = 0;
    *model = byte_two | (byte_one<<8);
    if (byte_one == 0xab || byte_one == 0xac)
    {
        *type = PS2_DEV_TYPE_KEYBOARD;
        goto done;
    }
    if (byte_two == 0xff)
    {
        // One-byte IDs should always be mice.
        *type = PS2_DEV_TYPE_MOUSE;
        goto done;
    }
    done:
    return;
}

void PS2_DetectDevice(ps2_port* port)
{
    uint8_t type = 0;

    // bool retried = false;
    // retry_echo:
    // PS2_DeviceWrite(port->second, 0xee);
    // uint8_t echo_response = PS2_DeviceRead(0xffff, nullptr);
    // if (echo_response == 0xfe)
    // {
    //     if (retried)
    //         return; // No device.
    //     retried = true;
    //     goto retry_echo;
    // }

    PS2_SendCommand(port, 0xf5, 0);

    uint16_t model = 0;
    identify_device(port, &model, &type);
    if ((type == PS2_DEV_TYPE_KEYBOARD && port->second) || (type == PS2_DEV_TYPE_MOUSE && !port->second))
    {
        // Try this again...
        uint16_t model2 = 0;
        uint8_t type2 = 0;
        identify_device(port, &model2, &type2);
        if (type2 == type && model2 == model)
            goto done;
        // Otherwise, choose the new ones.
        type = type2;
        model = model2;   
    }

    done:
    port->model = model;

    if (type != PS2_DEV_TYPE_UNKNOWN)
        OBOS_Log("PS/2: Found a %s on channel %c (model id: 0x%04x).\n", type == PS2_DEV_TYPE_KEYBOARD ? "keyboard" : "mouse", port->second ? '2' : '1', model);

    switch (type) {
        case PS2_DEV_TYPE_KEYBOARD:
            PS2_InitializeKeyboard(port);
            break;
        case PS2_DEV_TYPE_MOUSE:
            OBOS_Debug("PS/2: Found a PS/2 mouse, but PS/2 mice are unimplemented.\n");
            __attribute__((fallthrough)); 
        default:
            break;
    }
}