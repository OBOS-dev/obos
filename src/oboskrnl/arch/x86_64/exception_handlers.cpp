/*
	oboskrnl/arch/x86_64/exception_handlers.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/irq/interrupt_frame.h>
#include <arch/x86_64/irq/idt.h>

namespace obos
{
	void defaultExceptionHandler(interrupt_frame* frame)
	{
		uint32_t cpuId = 0, pid = -1, tid = -1;
		logger::panic(
			nullptr,
			"Exception %d at 0x%p (cpu %d, pid %d, tid %d). Error code: %d.\nDumping registers:\n"
			"\tRDI: 0x%p, RSI: 0x%p, RBP: 0x%p\n"
			"\tRSP: 0x%p, RBX: 0x%p, RDX: 0x%p\n"
			"\tRCX: 0x%p, RAX: 0x%p, RIP: 0x%p\n"
			"\t R8: 0x%p,  R9: 0x%p, R10: 0x%p\n"
			"\tR11: 0x%p, R12: 0x%p, R13: 0x%p\n"
			"\tR14: 0x%p, R15: 0x%p, RFL: 0x%p\n"
			"\t SS: 0x%p,  DS: 0x%p,  CS: 0x%p\n",
			frame->intNumber,
			frame->rip,
			cpuId,
			pid, tid,
			frame->errorCode,
			frame->rdi, frame->rsi, frame->rbp,
			frame->rsp, frame->rbx, frame->rdx,
			frame->rcx, frame->rax, frame->rip,
			frame->r8, frame->r9, frame->r10,
			frame->r11, frame->r12, frame->r13,
			frame->r14, frame->r15, frame->rflags,
			frame->ss, frame->ds, frame->cs
		);
	}
	void pageFaultHandler(interrupt_frame* frame)
	{
		uint32_t cpuId = 0, pid = -1, tid = -1;
		bool whileInScheduler = false;
		const char* action = (frame->errorCode & ((uintptr_t)1 << 1)) ? "write" : "read";
		if (frame->errorCode & ((uintptr_t)1 << 4))
			action = "execute";
		logger::panic(
			nullptr,
			"Page fault in %s-mode at 0x%p (cpu %d, pid %d, tid %d) while trying to %s a %s page. The address of this page is 0x%p. IRQL: %d. Error code: %d. whileInScheduler = %s\nDumping registers:\n"
			"\tRDI: 0x%p, RSI: 0x%p, RBP: 0x%p\n"
			"\tRSP: 0x%p, RBX: 0x%p, RDX: 0x%p\n"
			"\tRCX: 0x%p, RAX: 0x%p, RIP: 0x%p\n"
			"\t R8: 0x%p,  R9: 0x%p, R10: 0x%p\n"
			"\tR11: 0x%p, R12: 0x%p, R13: 0x%p\n"
			"\tR14: 0x%p, R15: 0x%p, RFL: 0x%p\n"
			"\t SS: 0x%p,  DS: 0x%p,  CS: 0x%p\n"
			"\tCR0: 0x%p, CR2: 0x%p, CR3: 0x%p\n"
			"\tCR4: 0x%p, CR8: 0x%p, EFER: 0x%p\n",
			(frame->errorCode & ((uintptr_t)1 << 2)) ? "user" : "kernel",
			frame->rip,
			cpuId,
			pid, tid,
			action,
			(frame->errorCode & ((uintptr_t)1 << 0)) ? "present" : "non-present",
			getCR2(),
			getCR8(),
			frame->errorCode,
			whileInScheduler ? "true" : "false",
			frame->rdi, frame->rsi, frame->rbp,
			frame->rsp, frame->rbx, frame->rdx,
			frame->rcx, frame->rax, frame->rip,
			frame->r8, frame->r9, frame->r10,
			frame->r11, frame->r12, frame->r13,
			frame->r14, frame->r15, frame->rflags,
			frame->ss, frame->ds, frame->cs,
			getCR0(), getCR2(), getCR3(),
			getCR4(), getCR8(), getEFER()
		);
	}
	
	void RegisterExceptionHandlers()
	{
		for (uint8_t vec = 0; vec < 14; vec++)
			RawRegisterInterrupt(vec, (uintptr_t)defaultExceptionHandler);
		RawRegisterInterrupt(14, (uintptr_t)pageFaultHandler);
		for (uint8_t vec = 15; vec < 32; vec++)
			RawRegisterInterrupt(vec, (uintptr_t)defaultExceptionHandler);
	}
}