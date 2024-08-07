/*
 * oboskrnl/arch/m68k/interrupt_frame.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

typedef struct interrupt_frame 
{
	uint32_t intNumber;
	uint32_t vector; // intNumber-64
	uintptr_t usp;
	uintptr_t d0,d1,d2,d3,d4,d5,d6,d7;
	uintptr_t a0,a1,a2,a3,a4,a5,a6;
	uint16_t padding;
	uint16_t sr;
	uintptr_t pc;
	uint16_t unused;
} OBOS_PACK interrupt_frame;