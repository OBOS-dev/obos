/*
	oboskrnl/arch/x86_64/thr_context_info.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <arch/x86_64/irq/interrupt_frame.h>

#include <arch/x86_64/mm/pmap_l4.h>

#include <arch/x86_64/asm_helpers.h>

#include <vmm/pg_context.h>
#include <vmm/map.h>
#include <vmm/prot.h>

#include <scheduler/stack.h>

#include <arch/x86_64/thr_context_info.h>

#define GS_BASE  0xC0000101

namespace obos
{
	namespace arch
	{
		void SetupThreadContext(ThreadContextInfo* info, 
								scheduler::thr_stack* stack,
								uintptr_t entry, uintptr_t arg1, 
								bool isUsermode, 
								size_t stackSize, 
								vmm::Context* ctx)
		{
			info->frame.rip = entry;
			info->frame.rdi = arg1;
			info->frame.rflags = RFLAGS_INTERRUPT_ENABLE | RFLAGS_CPUID | (isUsermode ? RFLAGS_IOPL_3 : 0) | (1<<1);
			info->frame.cs = isUsermode ? 0x20 : 0x08;
			info->frame.ss = info->frame.ds = isUsermode ? 0x18 : 0x10;
			info->pm = ctx->GetContext()->getCR3();
			info->xsave_context = !isUsermode ? nullptr : new uint8_t[info->xsave_size];
			info->fs_base = 0;
			info->gs_base = isUsermode ? 0 : rdmsr(GS_BASE);
			stack->base = (uintptr_t)vmm::Allocate(ctx, nullptr, stackSize, vmm::FLAGS_GUARD_PAGE_LEFT | vmm::FLAGS_RESERVE | vmm::FLAGS_COMMIT, isUsermode ? vmm::PROT_USER : 0);
			stack->size = stackSize;
			info->frame.rsp = stack->base + stack->size;
		}
	}
}