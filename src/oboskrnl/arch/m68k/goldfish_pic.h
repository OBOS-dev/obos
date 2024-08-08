/*
 * oboskrnl/arch/m68k/goldfish_pic.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/m68k/interrupt_frame.h>

void Arch_PICClearPending();
void Arch_PICDisable(uint8_t line);
void Arch_PICEnable(uint8_t line);
uint8_t Arch_PICGetPendingCount();
uint32_t Arch_PICGetPending();
void Arch_PICRegisterIRQ(uint8_t line, uint8_t irq);
void Arch_PICMaskIRQ(uint8_t line, bool mask);
void Arch_PICHandleIRQ(interrupt_frame*);
void Arch_PICHandleSpurious(interrupt_frame*);