/*
	oboskrnl/arch/x86_64/exception_handlers.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/irq/interrupt_frame.h>
#include <arch/x86_64/irq/idt.h>

#include <irq/irql.h>

#include <arch/x86_64/mm/pmap_l4.h>
#include <arch/x86_64/mm/map.h>

#include <vmm/page_fault_reason.h>
#include <vmm/page_descriptor.h>

#include <locks/spinlock.h>

#include <arch/vmm_defines.h>

#include <scheduler/cpu_local.h>

#if OBOS_KDBG_ENABLED
#	include <arch/x86_64/kdbg/exception_handlers.h>
#endif

namespace obos
{
	namespace arch
	{
		struct page_fault_handler
		{
			void(*callback)(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd);
			vmm::PageFaultReason reason;
			bool hasToBeInUserMode;
			bool inUse;
		};
		struct page_fault_handler_node
		{
			page_fault_handler_node *next, *prev;
			page_fault_handler* data;
			bool inUse;
		};
		static page_fault_handler s_handlers[OBOS_MAX_PAGE_FAULT_HANDLERS];
		static locks::SpinLock s_handlerAllocatorLock;
		static page_fault_handler *allocate_handler()
		{
			s_handlerAllocatorLock.Lock();
			page_fault_handler* ret = nullptr;
			for (size_t i = 0; i < OBOS_MAX_PAGE_FAULT_HANDLERS; i++)
				if (!s_handlers[i].inUse)
					ret = &s_handlers[i];
			if (!ret)
				return nullptr;
			ret->inUse = true;
			s_handlerAllocatorLock.Unlock();
			return ret;
		}
		static void free_handler(page_fault_handler* handler)
		{
			if (!(handler >= s_handlers && handler < s_handlers + OBOS_MAX_PAGE_FAULT_HANDLERS))
				return;
			handler->inUse = false;
			memzero(handler, sizeof(*handler));
		}
		struct
		{
			page_fault_handler_node *head, *tail;
			size_t nHandlers;
		} s_pfHandlers;
		static page_fault_handler_node s_nodes[OBOS_MAX_PAGE_FAULT_HANDLERS];
		static locks::SpinLock s_nodeAllocatorLock;
		static page_fault_handler_node* allocate_node()
		{
			s_nodeAllocatorLock.Lock();
			page_fault_handler_node* ret = nullptr;
			for (size_t i = 0; i < OBOS_MAX_PAGE_FAULT_HANDLERS; i++)
				if (!s_nodes[i].inUse)
					ret = &s_nodes[i];
			if (!ret)
				return nullptr;
			ret->inUse = true;
			s_nodeAllocatorLock.Unlock();
			return ret;
		}
		static void free_node(page_fault_handler_node* node)
		{
			if (!(node >= s_nodes && node < s_nodes + OBOS_MAX_PAGE_FAULT_HANDLERS))
				return;
			node->inUse = false;
			memzero(node, sizeof(*node));
		}

		bool register_page_fault_handler(vmm::PageFaultReason reason, bool hasToBeInUserMode, void(*callback)(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd))
		{
			if (s_pfHandlers.nHandlers == OBOS_MAX_PAGE_FAULT_HANDLERS)
				return false;
			page_fault_handler* handler = allocate_handler();
			page_fault_handler_node* node = allocate_node();
			if (!handler || !node)
			{
				if (handler)
					free_handler(handler);
				if (node)
					free_node(node);
				return false;
			}
			handler->hasToBeInUserMode = hasToBeInUserMode;
			handler->callback = callback;
			handler->reason = reason;
			node->data = handler;
			if (!s_pfHandlers.head)
				s_pfHandlers.head = node;
			if (s_pfHandlers.tail)
				s_pfHandlers.tail->next = node;
			node->prev = s_pfHandlers.tail;
			s_pfHandlers.tail = node;
			s_pfHandlers.nHandlers++;
			return true;
		}
		extern bool g_halt;
	}
	uint32_t getCPUId()
	{
		if (!scheduler::GetCPUPtr())
			return 0; // We're on the BSP
		return scheduler::GetCPUPtr()->cpuId;
	}
	uint32_t getTID()
	{
		if (scheduler::GetCPUPtr() && scheduler::GetCPUPtr()->currentThread)
			return scheduler::GetCPUPtr()->currentThread->tid;
		return (uint32_t)-1;
	}
	uint32_t getPID()
	{
		return 0;
	}
	void defaultExceptionHandler(interrupt_frame* frame)
	{
		uint8_t oldIRQL = 0;
		RaiseIRQL(IRQL_MASK_ALL, &oldIRQL);
		uint32_t cpuId = getCPUId(), pid = getPID(), tid = getTID();
		bool whileInScheduler = false;
#if OBOS_KDBG_ENABLED
		if (!kdbg::exceptionHandler(frame))
		{
			LowerIRQL(oldIRQL);
			return;
		}
#endif
		logger::panic(
			nullptr,
			"Exception %ld in %s-mode at 0x%016lx (cpu %d, pid %d, tid %d). IRQL: %d. Error code: %ld. whileInScheduler = %s\nDumping registers:\n"
			"\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
			"\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
			"\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
			"\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
			"\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
			"\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
			"\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
			"\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
			"\tCR4: 0x%016lx, CR8: 0x%016lx, EFER: 0x%016lx\n",
			frame->intNumber,
			(frame->cs != 0x8) ? "user" : "kernel",
			frame->rip,
			cpuId,
			pid, tid,
			GetIRQL(),
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
			getCR4(), oldIRQL , getEFER()
		);
		asm volatile("nop");
	}
	bool callPageFaultHandlers(vmm::PageFaultReason reason, uintptr_t at, const vmm::page_descriptor &pd, uint32_t ec)
	{
		arch::page_fault_handler_node* node = arch::s_pfHandlers.head;
		while (node)
		{
			if (!node->data)
				goto down1;
			if (node->data->hasToBeInUserMode && !(ec & vmm::PageFault_InUserMode))
				goto down1;
			if (node->data->reason == reason)
				node->data->callback((void*)at, (vmm::PageFaultErrorCode)ec, pd);
		down1:
			node = node->next;
		}
		if (ec & vmm::PageFault_InUserMode && reason == vmm::PageFaultReason::PageFault_AccessViolation)
			return false;
		if (ec & vmm::PageFault_DemandPage || reason == vmm::PageFaultReason::PageFault_DemandPaging)
			return false;
		return true;
	}
	uint32_t DecodePFErrorCode(uintptr_t ec)
	{
		uintptr_t ret = 0;
		if (ec & 1)
			ret |= vmm::PageFault_IsPresent;
		if (ec & 2)
			ret |= vmm::PageFault_Write;
		else
			ret |= vmm::PageFault_Read;
		if (ec & 4)
			ret |= vmm::PageFault_InUserMode;
		if (ec & 16)
			ret |= vmm::PageFault_Execution;
		return ret;
	}
	void pageFaultHandler(interrupt_frame* frame)
	{
		uint8_t oldIRQL = 0;
		RaiseIRQL(15, &oldIRQL);
		arch::PageMap* pm = (arch::PageMap*)getCR3();
		uintptr_t at = getCR2();
		if (frame->errorCode & 1)
		{
			vmm::page_descriptor pd = {};
			arch::get_page_descriptor(pm, (void*)at, pd);
			uintptr_t entry = 0;
			if (pd.isHugePage)
				entry = pm->GetL2PageMapEntryAt(at);
			else
				entry = pm->GetL1PageMapEntryAt(at);
			if (!(entry & ((uintptr_t)1 << 9)))
				goto fault;
			uint32_t ec = DecodePFErrorCode(frame->errorCode);
			ec |= vmm::PageFault_DemandPage;
			if (!callPageFaultHandlers(vmm::PageFaultReason::PageFault_DemandPaging, at, pd, ec))
			{
				LowerIRQL(oldIRQL);
				return;
			}
		}
	fault:
		
		// Call page fault handlers.
		vmm::page_descriptor pd = {};
		arch::get_page_descriptor(pm, (void*)at, pd);
		uint32_t ec = DecodePFErrorCode(frame->errorCode);
		if (!callPageFaultHandlers(vmm::PageFaultReason::PageFault_AccessViolation, at, pd, ec))
		{
			LowerIRQL(oldIRQL);
			return;
		}
#if OBOS_KDBG_ENABLED
		if (!kdbg::exceptionHandler(frame))
		{
			LowerIRQL(oldIRQL);
			return;
		}
#endif
		uint32_t cpuId = getCPUId(), pid = getPID(), tid = getTID();
		bool whileInScheduler = false;
		const char* action = (frame->errorCode & ((uintptr_t)1 << 1)) ? "write" : "read";
		if (frame->errorCode & ((uintptr_t)1 << 4))
			action = "execute";
		logger::panic(
			nullptr,
			"Page fault in %s-mode at 0x%016lx (cpu %d, pid %d, tid %d) while trying to %s a %s page. The address of this page is 0x%016lx. IRQL: %d. Error code: %ld. whileInScheduler = %s\nDumping registers:\n"
			"\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
			"\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
			"\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
			"\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
			"\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
			"\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
			"\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
			"\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
			"\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
			(frame->errorCode & ((uintptr_t)1 << 2)) ? "user" : "kernel",
			frame->rip,
			cpuId,
			pid, tid,
			action,
			(frame->errorCode & ((uintptr_t)1 << 0)) ? "present" : "non-present",
			getCR2(),
			oldIRQL,
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
			getCR4(), oldIRQL , getEFER()
		);
		asm volatile("nop");
	}
	void nmiHandler(interrupt_frame* frame)
	{
		uint8_t oldIrql = 0;
		RaiseIRQL(0xf, &oldIrql);
#if OBOS_KDBG_ENABLED
		kdbg::exceptionHandler(frame);
#endif
		if (arch::g_halt)
			while (1)
				asm volatile("hlt");
		logger::panic(nullptr, "NMI thrown by hardware! System Control Port: 0x%x.\n", (uint16_t)inb(0x92) | ((uint16_t)inb(0x61) << 8));
	}
	
	void RegisterExceptionHandlers()
	{
		RawRegisterInterrupt(0, (uintptr_t)defaultExceptionHandler);
		RawRegisterInterrupt(1, (uintptr_t)defaultExceptionHandler);
		RawRegisterInterrupt(2, (uintptr_t)nmiHandler);
		for (uint8_t vec = 3; vec < 14; vec++)
			RawRegisterInterrupt(vec, (uintptr_t)defaultExceptionHandler);
		RawRegisterInterrupt(14, (uintptr_t)pageFaultHandler);
		for (uint8_t vec = 15; vec < 32; vec++)
			RawRegisterInterrupt(vec, (uintptr_t)defaultExceptionHandler);
	}
}