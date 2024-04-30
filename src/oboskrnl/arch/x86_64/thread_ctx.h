/*
*	oboskrnl/arch/x86_64/thread_ctx.asm
*	
*	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <arch/x86_64/interrupt_frame.h>

// Don't forget to update this in the assembly code (thread_ctx.asm)
struct __thread_context_info
{
	void* extended_ctx_ptr;
	uintptr_t cr3;
	uint8_t irql;
	uint64_t gs_base, fs_base;
	interrupt_frame frame;
} OBOS_ALIGN(8);