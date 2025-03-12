/*
 * generic/libps2/detect.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include "controller.h"

void PS2_DetectDevice(ps2_port* port);

uint8_t PS2_SendCommand(ps2_port* port, uint8_t cmd, size_t nArgs, ...);