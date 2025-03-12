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
    return PS2_DeviceRead(0xffff, status);
}

OBOS_PAGEABLE_FUNCTION uint8_t PS2_SendCommand(ps2_port* port, uint8_t cmd, size_t nArgs, ...)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    va_list list;
    va_start(list, nArgs);
    uint8_t res = send_command_impl(port, &status, cmd, nArgs, list);
    if (obos_is_error(status))
    {
        OBOS_Warning("Timeout while waiting for a response from the PS/2 Keyboard. Aborting\n");
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

void PS2_DetectDevice(ps2_port* port)
{
    OBOS_UNUSED(port);
}