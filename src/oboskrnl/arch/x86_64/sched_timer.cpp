/*
	oboskrnl/arch/x86_64/sched_timer.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/cpu_local.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/sched_timer.h>

#include <arch/x86_64/irq/apic.h>

#include <arch/x86_64/hpet_table.h>

namespace obos
{
	namespace arch
	{
		extern Irq g_ipiIrq;
		uint64_t g_hpetFrequency = 0;
		extern "C" uint64_t calibrateHPET(uint64_t freq)
		{
			if (!g_hpetFrequency)
				g_hpetFrequency = 1000000000000000/g_hpetAddress->generalCapabilitiesAndID.counterCLKPeriod;
			g_hpetAddress->generalConfig &= ~(1<<0);
			uint64_t compValue = g_hpetAddress->mainCounterValue + (g_hpetFrequency/freq);
			g_hpetAddress->timer0.timerConfigAndCapabilities &= ~(1<<2);
			g_hpetAddress->timer0.timerConfigAndCapabilities &= ~(1<<3);
			return compValue;
		}
		bool LAPICTimerIRQChecker(const Irq*, const struct IrqVector*, void* _udata)
		{
			uintptr_t* udata = (uintptr_t*)_udata;
			const uint64_t &freq = udata[0];
			uint64_t &lastHPETCounter = udata[1];
			uint64_t &expectedHPETCounter = udata[2];
			g_hpetAddress->generalConfig &= ~(1<<0);
			bool status = false;
			lastHPETCounter = g_hpetAddress->mainCounterValue;
			if (lastHPETCounter >= expectedHPETCounter)
			{
				// It is likely that this is a LAPIC timer interrupt, as the amount of time passed since the last interrupt on the IRQ 
				// is equal or greater to the frequency of the LAPIC timer.
				status = expectedHPETCounter != 0;
				expectedHPETCounter = lastHPETCounter + (g_hpetFrequency/freq) - (g_hpetFrequency/(freq*2));
			}
			g_hpetAddress->generalConfig |= (1<<0);
			return status;
		}
		void StartTimerOnCPU(scheduler::cpu_local* cpu, uint64_t freqHz, Irq& irq)
		{
			if (!g_hpetFrequency)
				g_hpetFrequency = 1000000000000000/g_hpetAddress->generalCapabilitiesAndID.counterCLKPeriod;
			auto udata = new uint64_t[3];
			g_hpetAddress->generalConfig &= ~(1<<0);
			udata[0] = freqHz;
			udata[1] = g_hpetAddress->mainCounterValue;
			udata[2] = udata[1] + (g_hpetFrequency/freqHz);
			g_hpetAddress->generalConfig |= (1<<0);
			irq.SetIRQChecker(LAPICTimerIRQChecker, udata);
			// Setup the IPI.
			ipi* tIPI = new ipi{};
			timer_ipi* payload = new timer_ipi{};
			tIPI->data.timer = payload;
			tIPI->type = ipi::IPI_TIMER;
			// Set the IRQ that the timer shall happen on.
			payload->irq = &irq;
			// Set the target CPU's timer frequency.
			payload->freq = freqHz;
			// The scheduler expects the timer to be periodic.
			payload->singleShoot = false;
			// Add the IPI to the queue and call it.
			cpu->archSpecific.ipi_queue.push(tIPI);
			uint8_t oldIRQL = IRQL_INVALID;
			if (cpu == scheduler::GetCPUPtr() && GetIRQL() >= IRQL_IPI_DISPATCH)
			{
				// We need to temporarily lower the IRQL for self-ipis, as otherwise we would deadlock while waiting.
				oldIRQL = GetIRQL();
				LowerIRQL(IRQL_DISPATCH);
			}
			LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::Fixed, g_ipiIrq.GetVector() + 0x20, cpu->cpuId);
			// Wait for the IPI to be processed.
			// We do this as we need to free the structures, and we don't want to cause a race condition.
			while(!tIPI->processed)
				pause();
			if (oldIRQL != IRQL_INVALID)			
			{
				uint8_t _oldIRQL = 0;
				RaiseIRQL(oldIRQL, &_oldIRQL);
			}
			// After the IPI has been processed, we must clean up after ourselves.
			// The IPI isn't in the queue anymore, so we just have to free the payload and ipi object.
			delete tIPI;
			delete payload;
		}
	}
}