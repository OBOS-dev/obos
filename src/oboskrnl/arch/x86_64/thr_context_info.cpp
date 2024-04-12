/*
	oboskrnl/arch/x86_64/thr_context_info.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <memmanip.h>

#include <arch/x86_64/irq/interrupt_frame.h>

#include <arch/x86_64/mm/pmap_l4.h>

#include <arch/x86_64/asm_helpers.h>

#include <vmm/pg_context.h>
#include <vmm/map.h>
#include <vmm/prot.h>

#include <scheduler/stack.h>
#include <scheduler/cpu_local.h>

#include <arch/x86_64/thr_context_info.h>

#include <arch/x86_64/kdbg/init.h>

#include <irq/irql.h>

#define FS_BASE  		0xC0000100
#define GS_BASE  		0xC0000101
#define KERNEL_GS_BASE  0xC0000102

extern "C" uintptr_t GetContextInfoOffset(obos::scheduler::Thread*)
{
	return (uintptr_t)&((obos::scheduler::Thread*)nullptr)->context;
}
extern "C" uintptr_t GetLastPreemptTimeOffset(obos::scheduler::Thread*)
{
	return (uintptr_t)&((obos::scheduler::Thread*)nullptr)->lastPreemptTime;
}
extern "C" void RaiseIRQLForScheduler()
{
	uint8_t oldIRQL = 0;
	if (obos::GetIRQL() < 2)
		obos::RaiseIRQL(2, &oldIRQL, false);
}

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
#if OBOS_KDBG_ENABLED
			if (kdbg::g_initialized)
				info->frame.rflags |= RFLAGS_TRAP;
#endif
			info->frame.cs = isUsermode ? 0x20 : 0x08;
			info->frame.ss = info->frame.ds = isUsermode ? 0x18 : 0x10;
			info->pm = ctx->GetContext()->getCR3();
			info->xsave_context = !isUsermode ? nullptr : new uint8_t[info->xsave_size];
			info->fs_base = 0;
			info->gs_base = isUsermode ? 0 : rdmsr(GS_BASE);
			stack->base = (uintptr_t)vmm::Allocate(ctx, nullptr, stackSize, vmm::FLAGS_GUARD_PAGE_LEFT | vmm::FLAGS_RESERVE | vmm::FLAGS_COMMIT, isUsermode ? vmm::PROT_USER : 0);
			stack->size = stackSize;
			info->frame.rsp = stack->base + stack->size;
			info->irql = 0;
		}
		void SaveThreadContext(ThreadContextInfo* dest, interrupt_frame* frame)
		{
			memcpy(&dest->frame, frame, sizeof(*frame));
			dest->pm = (PageMap*)getCR3();
			dest->gs_base = rdmsr(KERNEL_GS_BASE);
			dest->fs_base = rdmsr(FS_BASE);
			dest->pm = (PageMap*)getCR3();
			if (dest->xsave_context)
				xsave(dest->xsave_context);
		}
		
	}
}