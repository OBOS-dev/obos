/*
 * drivers/x86/bochs_vbe/io.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

#define INDEX_ID 0
#define INDEX_XRES 1
#define INDEX_YRES 2
#define INDEX_BPP 3
#define INDEX_ENABLE 4
#define INDEX_BANK 5
#define INDEX_VIRT_WIDTH 6
#define INDEX_VIRT_HEIGHT 7
#define INDEX_X_OFFSET 8
#define INDEX_Y_OFFSET 9

void WriteRegister(uint16_t index, uint16_t val);
uint16_t ReadRegister(uint16_t index);
