/*
 * drivers/x86/bochs_vbe/io.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

#include <arch/x86_64/asm_helpers.h>

#include "io.h"

#define INDEX_REG 0x1ce
#define DATA_REG  0x1cf

void WriteRegister(uint16_t index, uint16_t val)
{
    outw(INDEX_REG, index);
    outw(DATA_REG, val);
}
uint16_t ReadRegister(uint16_t index)
{
    outw(INDEX_REG, index);
    return inw(DATA_REG);
}
