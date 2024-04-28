/*
	oboskrnl/scheduler/init.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <scheduler/init.h>
#include <scheduler/scheduler.h>
#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>

#include <vmm/init.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/irq_register.h>
#include <arch/sched_timer.h>

namespace obos
{
	namespace scheduler
	{
		extern "C" void idleTask(cpu_local* cpu);
		bool g_initialized;
		uint32_t g_nextTID = 1;
		ThrAffinity g_defaultAffinity = 0;
		Irq g_schedulerIRQ{ 2, false };
		void sched_timer_int(const Irq*, const IrqVector*, void*, interrupt_frame*)
		{
			// uint8_t oldIRQL = 0xff;
			// if (GetIRQL() < 2)
				// RaiseIRQL(2, &oldIRQL, false);
			if (GetCPUPtr()->isBSP)
				g_ticks++; 
			yield();
			// if (oldIRQL != 0xff)
				// LowerIRQL(oldIRQL, false);
		}
		bool InitializeScheduler()
		{
			if (g_initialized)
				return false;
			new (&g_schedulerIRQ) Irq{ 2, false };
			g_schedulerIRQ.SetHandler(sched_timer_int, nullptr);
			// Start the idle threads and the timer on all CPUs.
			for (size_t i = 0; i < g_nCPUs; i++)
			{
				g_defaultAffinity |= (1<<g_cpuInfo[i].cpuId);
				Thread* thr = new Thread{};
				thr->tid = g_nextTID++;
				thr->referenceCount = 0;
				
				thr->priority = ThreadPriority::Idle;
				thr->ogAffinity = thr->affinity = (1<<g_cpuInfo[i].cpuId);
				thr->status = ThreadStatus::CanRun;
				g_threadPriorities[(int)thr->priority].Append(thr);
				
				thr->addressSpace = &vmm::g_kernelContext;
				arch::SetupThreadContext(&thr->context, &thr->thread_stack, (uintptr_t)idleTask, (uintptr_t)&g_cpuInfo[i], false, 0x4000, thr->addressSpace);
				g_cpuInfo[i].idleThread = thr;
				arch::StartTimerOnCPU(&g_cpuInfo[i], g_schedulerFrequency, g_schedulerIRQ);
			}
			g_initialized = true;
			return true;
		}
		bool StartKernelMainThread(void(*entry)())
		{
			if (!g_initialized)
				return false;
			Thread* thr = new Thread{};
			thr->tid = 0;
			thr->referenceCount = 0;
			
			thr->priority = ThreadPriority::Normal;
#ifdef OBOS_DEBUG
			thr->ogAffinity = g_defaultAffinity;
#else
			thr->ogAffinity = g_defaultAffinity;
#endif
			thr->affinity = thr->ogAffinity;
			thr->status = ThreadStatus::CanRun;
			
			thr->addressSpace = &vmm::g_kernelContext;
			arch::SetupThreadContext(&thr->context, &thr->thread_stack, (uintptr_t)entry, 0, false, 0x10000, thr->addressSpace);
			g_threadPriorities[(int)thr->priority].Append(thr);
			return true;
		}
	}
}