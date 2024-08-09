/*
 * oboskrnl/arch/m68k/thread_ctx.asm
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <arch/m68k/interrupt_frame.h>

#include <irq/irql.h>

struct thread_context_info
{
	// 0x00
	uintptr_t urp;
	// 0x04
	uintptr_t sp;
	// 0x08,0x0C,0x10,0x14,0x18,0x2C,0x30,0x34
	uintptr_t d0,d1,d2,d3,d4,d5,d6,d7;
	// 0x38,0x3C,0x40,0x44,0x48,0x4C,0x50
	uintptr_t a0,a1,a2,a3,a4,a5,a6;
	// 0x54
	uint16_t padding;
	// 0x56
	uint16_t sr;
	// 0x58
	uintptr_t pc;
	// 0x5c
	uint16_t unused;
	// 0x5e
	irql irql;
	// 0x61
	void* stackBase;
	// 0x65
	size_t stackSize;
} OBOS_ALIGN(8);