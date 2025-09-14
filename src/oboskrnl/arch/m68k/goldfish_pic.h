/*
 * oboskrnl/arch/m68k/goldfish_pic.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/m68k/interrupt_frame.h>

typedef struct pic_irq {
    uint8_t vector;
    bool masked;
} pic_irq;
typedef struct pic
{
    uintptr_t base;
    uintptr_t phys_base;
    pic_irq irqs[32];
} pic;
extern pic* Arch_PICBases;
extern size_t Arch_PICCount;
void Arch_PICClearPending(pic* on);
void Arch_PICDisable(pic*, uint32_t line);
void Arch_PICEnable(pic*, uint32_t line);
uint8_t Arch_PICGetPendingCount(pic*);
uint32_t Arch_PICGetPending(pic*);
void Arch_PICRegisterIRQ(uint32_t line, uint8_t irq);
void Arch_PICUnregisterIRQ(uint32_t line);
void Arch_PICMaskIRQ(uint32_t line, bool mask);
void Arch_PICHandleIRQ(interrupt_frame*);
void Arch_PICHandleSpurious(interrupt_frame*);