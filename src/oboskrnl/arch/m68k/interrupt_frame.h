/*
 * oboskrnl/arch/m68k/interrupt_frame.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

struct OBOS_PACK ssw_68040 {
	uint16_t cp : 1;
	uint16_t cu : 1;
	uint16_t ct : 1;
	uint16_t cm : 1;
	uint16_t ma : 1;
	uint16_t atc : 1;
	uint16_t lk : 1;
	uint16_t rw : 1;
	uint16_t x : 1;
	uint16_t size : 2;
	uint16_t tt : 2;
	uint16_t tm : 3;
};
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
	struct format_7 
	{
		// Effective address (whatever that means)
		uint32_t ea;
		struct ssw_68040 ssw;
		uint16_t wb3s, wb2s, wb1s;
		// Fault address
		uint32_t fa;
		uint32_t wb3a, wb3d, wb2a, fb2d, wb1a, wb1d;
		uint32_t pd1, pd2, pd3;
	} OBOS_PACK format_7;
} OBOS_PACK interrupt_frame;