/*
	oboskrnl/arch/x86_64/thr_context_info.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/irq/interrupt_frame.h>

#include <arch/x86_64/mm/pmap_l4.h>

#include <vmm/pg_context.h>

#include <scheduler/stack.h>

namespace obos
{
	namespace arch
	{
		struct ThreadContextInfo
		{
			static size_t xsave_size;
			interrupt_frame frame;
			PageMap *pm;
			// Must be at least 576 bytes and aligned to 64 bytes, or nullptr for kernel-mode threads.
			uint8_t* xsave_context;
			uintptr_t gs_base, fs_base;
		};
		[[noreturn]] void SwitchToThrContext(ThreadContextInfo* info);
		void SetupThreadContext(ThreadContextInfo* info, 
								scheduler::thr_stack* stack,
								uintptr_t entry, uintptr_t arg1, 
								bool isUsermode, 
								size_t stackSize, 
								vmm::Context* ctx);
	}
}