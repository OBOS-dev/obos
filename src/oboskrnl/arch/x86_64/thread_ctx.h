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
struct thread_context_info
{
	void* extended_ctx_ptr;
	uint8_t irql;
	uintptr_t cr3;
	uint64_t gs_base, fs_base;
	interrupt_frame frame;
	void* stackBase;
	size_t stackSize;
} OBOS_ALIGN(8);
