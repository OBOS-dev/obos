/*
	oboskrnl/arch/x86_64/irq/ipi.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <arch/x86_64/cpu_local_arch.h>
#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/irq/interrupt_frame.h>
#include <arch/x86_64/irq/apic.h>

#include <scheduler/cpu_local.h>

#include <irq/irq.h>
#include <irq/irql.h>

extern "C" uint64_t FindCounter(uint64_t hz);

namespace obos
{
	namespace arch
	{
		Irq g_ipiIrq{ 3,false };
		void timer_ipi::_handler(base_ipi* _this, interrupt_frame*)
		{
			timer_ipi* obj = (timer_ipi*)_this;
			g_localAPICAddress->divideConfig = 0b1101;
			g_localAPICAddress->lvtTimer = (obj->irq->GetVector() + 0x20) | (((uint32_t)!obj->singleShoot) << 17);
			g_localAPICAddress->initialCount = FindCounter(obj->freq);
		}
		void shootdown_ipi::_handler(base_ipi* _this, interrupt_frame*)
		{
			shootdown_ipi* obj = (shootdown_ipi*)_this;
			if (getCR3() == (uintptr_t)obj->pm)
				invlpg(obj->virt);
		}
		void dbg_reg_ipi::_handler(base_ipi* _this, interrupt_frame*)
		{
			// uint8_t regIdx;
			// uint64_t* val;
			// // false to read from DRn into *val, true to write the value at *val into DRn. 
			// bool rw; 
			dbg_reg_ipi* obj = (dbg_reg_ipi*)_this;
			if (!obj->rw)
			{
				uint64_t ret = 0;
				switch (obj->regIdx)
				{
				case 0: asm volatile("mov %%dr0, %0" :"=r"(ret)::); break;
				case 1: asm volatile("mov %%dr1, %0" :"=r"(ret)::); break;
				case 2: asm volatile("mov %%dr2, %0" :"=r"(ret)::); break;
				case 3: asm volatile("mov %%dr3, %0" :"=r"(ret)::); break;
				case 6:
				[[fallthrough]]; case 4: asm volatile("mov %%dr6, %0" :"=r"(ret)::); break;
				case 7:
				[[fallthrough]]; case 5: asm volatile("mov %%dr7, %0" :"=r"(ret)::); break;
				default: ret = 0xffff'ffff'ffff'ffff;
				}
				*obj->val = ret;
			}
			else
			{
				uint64_t val = *obj->val;
				switch (obj->regIdx)
				{
				case 0: asm volatile("mov %0, %%dr0" ::"r"(val):); break;
				case 1: asm volatile("mov %0, %%dr1" ::"r"(val):); break;
				case 2: asm volatile("mov %0, %%dr2" ::"r"(val):); break;
				case 3: asm volatile("mov %0, %%dr3" ::"r"(val):); break;
				case 6:
				[[fallthrough]]; case 4: asm volatile("mov %0, %%dr6" ::"r"(val):); break;
				case 7:
				[[fallthrough]]; case 5: asm volatile("mov %0, %%dr7" ::"r"(val):); break;
				}
			}
		}
		// Required IRQL: 0x3
		void IpiHandler(const Irq*, const IrqVector*, void*, interrupt_frame* frame)
		{
			uint8_t oldIRQL = 0xff;
			if (GetIRQL() < 3)
				RaiseIRQL(0x3, &oldIRQL);
			ipi* cur = scheduler::GetCPUPtr()->archSpecific.ipi_queue.pop();
			if (!cur)
				return; // No IPI to handle.
			OBOS_ASSERTP(cur->type != ipi::IPI_INVALID, "IPI called with invalid type.");
			OBOS_ASSERTP(cur->data.base->handler != nullptr, "IPI called with null handler.\n");
			if (cur->data.base->handler == nullptr)
			{
				if (oldIRQL != 0xff)
					LowerIRQL(oldIRQL);
				return;
			}
			if (cur->type == ipi::IPI_INVALID)
			{
				if (oldIRQL != 0xff)
					LowerIRQL(oldIRQL);
				return;
			}
			cur->data.base->handler(cur->data.base, frame);
			cur->processed = true;
			if (oldIRQL != 0xff)
				LowerIRQL(oldIRQL);
		}
		bool IpiChecker(const Irq*, const struct IrqVector*, void*)
		{
			return scheduler::GetCPUPtr()->archSpecific.ipi_queue.nNodes > 0 /* If there are any pending IPIs */;
		}
		void RegisterIPIHandler()
		{
			new (&g_ipiIrq) Irq{ 3, false };
			g_ipiIrq.SetIRQChecker(IpiChecker, nullptr);
			g_ipiIrq.SetHandler(IpiHandler, nullptr);
		}
	}
}