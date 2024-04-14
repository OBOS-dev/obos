/*
	oboskrnl/arch/x86_64/thr_context_info.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

#include <arch/x86_64/irq/interrupt_frame.h>

#include <arch/x86_64/mm/pmap_l4.h>

#include <vmm/pg_context.h>

#include <scheduler/stack.h>

namespace obos
{
	namespace scheduler
	{
		struct Thread;
	}
	namespace arch
	{
		struct ThreadContextInfo
		{
			static size_t xsave_size;
			// Must be at least 576 bytes and aligned to 64 bytes, or nullptr for kernel-mode threads.
			uint8_t* xsave_context;
			PageMap *pm;
			uint64_t irql;
			uintptr_t gs_base, fs_base;
			interrupt_frame frame;
		};
		OBOS_EXPORT [[noreturn]] void SwitchToThrContext(ThreadContextInfo* info);
		OBOS_EXPORT void SetupThreadContext(ThreadContextInfo* info,
								scheduler::thr_stack* stack,
								uintptr_t entry, uintptr_t arg1, 
								bool isUsermode, 
								size_t stackSize, 
								vmm::Context* ctx);
		OBOS_EXPORT void SaveThreadContext(ThreadContextInfo* dest, interrupt_frame* frame, bool saveIRQL = true);
		// Saves the thread context with the CPU's current context and calls the scheduler.
		OBOS_EXPORT void YieldThread(scheduler::Thread *thr);
		OBOS_EXPORT void JumpToFunctionWithCPUTempStack(void(*func)(uintptr_t userdata), uintptr_t userdata);
	}
}