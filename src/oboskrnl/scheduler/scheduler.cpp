/*
	oboskrnl/scheduler/scheduler.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <allocators/slab.h>

#include <arch/thr_context_info.h>

#include <scheduler/init.h>
#include <scheduler/cpu_local.h>

#include <irq/irql.h>

namespace obos
{
	namespace scheduler
	{
		SchedulerTime g_ticks;
		ThreadList g_threadPriorities[4];
		bool check_thread_affinity(const Thread* thr)
		{
			return (thr->affinity & (1<<GetCPUPtr()->cpuId));
		}
		bool can_thread_run(const Thread* thr)
		{
			return check_thread_affinity(thr) && !((uint32_t)thr->flags & (uint32_t)ThreadFlags::IsDead) && thr->status == ThreadStatus::CanRun;
		}
		Thread* findThreadInList(ThreadList& list)
		{
			// Find a thread to run that has the following attributes:
			// status == ThreadStatus::CanRun
			// (affinity & (1<<GetCPUPtr()->cpuId)) == (1<<GetCPUPtr()->cpuId) (meaning the affinity says the thread can be run on the current CPU)
			// !(flags & ThreadFlags::IsDead)
			// The last preempt time must be the smallest out of all the threads in the list.
			Thread* chosenThread = nullptr;
			for (ThreadNode* curNode = list.head; curNode; )
			{
				auto thr = curNode->thr;
				if (!can_thread_run(thr))
					goto end;
				// The thread's runnable, but will it be the one to be run?
				if (!chosenThread || thr->lastPreemptTime < chosenThread->lastPreemptTime)
					chosenThread = thr;
				
				end:
				curNode = curNode->next;
			}
			return chosenThread;
		}
		void schedule()
		{
			if (!g_initialized)
				return;
			if (GetIRQL() < 2)
				logger::panic(nullptr, "%s: Scheduler must only be run at IRQL 2 or higher.\n", __func__);
			if (GetCPUPtr()->currentThread)
			{
				// Mark the current thread as ThreadStatus::CanRun if it is in the ThreadStatus::Running state.
				auto thr = GetCPUPtr()->currentThread;
				if (thr->status == ThreadStatus::Running)
					thr->status = ThreadStatus::CanRun;
				thr->affinity = thr->ogAffinity;
			}
			// Run the first (runnable) DPC in the list.
			ThreadList& dpcList = GetCPUPtr()->dpcList;
			Thread* dpc = nullptr;
			for (ThreadNode* curNode = dpcList.head; curNode; )
			{
				auto thr = curNode->thr;
				if (!can_thread_run(thr))
					goto end;
				dpc = thr;
				break;
				end:
				curNode = curNode->next;
			}
			Thread* chosenThread = nullptr;
			if (dpc)
			{ 
				chosenThread = dpc;
				goto found;
			}
			// We got past the DPC stage, choose a thread to run.
			for (int i = sizeof(g_threadPriorities) / sizeof(g_threadPriorities[0]) - 1; i >= 0; i--)
			{
				// Make sure no one else chooses the same thread as us.
				g_threadPriorities[i].lock.Lock();
				Thread* thr = findThreadInList(g_threadPriorities[i]);
				if (thr)
				{
					chosenThread = thr;
					chosenThread->affinity = (1<<GetCPUPtr()->cpuId);
					g_threadPriorities[i].lock.Unlock();
					break;
				}
				g_threadPriorities[i].lock.Unlock();
			}
			if (!chosenThread)
				chosenThread = GetCPUPtr()->idleThread;
			if (!chosenThread)
			{
				if (GetCPUPtr()->currentThread)
					if (can_thread_run(GetCPUPtr()->currentThread))
						chosenThread = GetCPUPtr()->currentThread; // Run the current thread.
				if (!chosenThread)
					logger::panic(nullptr, "%s, cpu %d: Could not find a thread to run, the idle thread doesn't exist, and the current thread cannot be run.\n", __func__, GetCPUPtr()->cpuId);
			}
			found:
			chosenThread->status = ThreadStatus::Running;
			chosenThread->affinity = (1<<GetCPUPtr()->cpuId);
			GetCPUPtr()->currentThread = chosenThread;
			arch::SwitchToThrContext(&chosenThread->context);
		}
		void yield()
		{
			if (!GetCPUPtr()->currentThread)
			{
				uint8_t oldIRQL = 0xff;
				if (GetIRQL() < 2)
					RaiseIRQL(2, &oldIRQL, false);
				// No need to worry about the IRQL not being restored, as it's going to be discarded anyway.
				schedule();
				if (oldIRQL != 0xff)
					LowerIRQL(oldIRQL);
				if (g_initialized)
					logger::panic(nullptr, "schedule() returned.\n");
				return;
			}
			arch::YieldThread(GetCPUPtr()->currentThread);
		}
	}
}