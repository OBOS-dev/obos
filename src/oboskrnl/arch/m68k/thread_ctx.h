/*
 * oboskrnl/arch/m68k/thread_ctx.asm
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <arch/m68k/interrupt_frame.h>

struct thread_context_info
{
	uintptr_t urp;
	interrupt_frame frame;
	void* stackBase;
	size_t stackSize;
} OBOS_ALIGN(8);