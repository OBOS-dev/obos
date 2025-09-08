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
	uintptr_t urp;
	uintptr_t sp;
	uintptr_t usp;
	uintptr_t d0,d1,d2,d3,d4,d5,d6,d7;
	uintptr_t a0,a1,a2,a3,a4,a5,a6;
	uint16_t padding;
	uint16_t sr;
	uintptr_t pc;
	uint16_t unused;
	irql irql;
	void* stackBase;
	size_t stackSize;
	void* tcb;
} OBOS_ALIGN(4);