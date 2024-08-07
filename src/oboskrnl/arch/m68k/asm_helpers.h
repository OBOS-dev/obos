/*
 * oboskrnl/arch/m68k/asm_helpers.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

void setSR(uint16_t to);
uint16_t getSR();