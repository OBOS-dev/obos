/*
 * oboskrnl/scheduler/schedule.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <signal.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

#include <irq/irql.h>
#include <irq/timer.h>

#include <locks/spinlock.h>

#ifdef __x86_64__
#	include <arch/x86_64/lapic.h>
#endif

#define getCurrentThread (CoreS_GetCPULocalPtr()->currentThread)
#define getIdleThread (CoreS_GetCPULocalPtr()->idleThread)
#define getSchedulerTicks (CoreS_GetCPULocalPtr()->schedulerTicks)
#define getPriorityLists (CoreS_GetCPULocalPtr()->priorityLists)
#define priorityList(priority) ((priority <= THREAD_PRIORITY_MAX_VALUE) ? &getPriorityLists[priority] : nullptr)
#define verifyAffinity(thr, cpuId) (thr->affinity & CoreH_CPUIdToAffinity(cpuId))
#define threadCanRunThread(thr) ((thr->status == THREAD_STATUS_RUNNING || thr->status == THREAD_STATUS_READY) && verifyAffinity(thr, CoreS_GetCPULocalPtr()->id))

size_t Core_ReadyThreadCount;
const uint64_t Core_ThreadPriorityToQuantum[THREAD_PRIORITY_MAX_VALUE+1] = {
	2, // THREAD_PRIORITY_IDLE
	4, // THREAD_PRIORITY_LOW
	8, // THREAD_PRIORITY_NORMAL
	12, // THREAD_PRIORITY_HIGH
	12, // THREAD_PRIORITY_URGENT
	UINT64_MAX, // THREAD_PRIORITY_REAL_TIME
};

__attribute__((no_instrument_function)) OBOS_WEAK thread* Core_GetCurrentThread() { if (!CoreS_GetCPULocalPtr()) return nullptr; return getCurrentThread; }

/*
 * The scheduler is the thing that chooses a thread to run.
 * Many hobby OS developers think that's all it should do.
 * That is wrong, or rather that's not all it should do, in my opinion.
 * It is also a priority manager, it must make sure no threads starve by temporarily raising their priority.
 * The scheduler must do load balancing.
 * TODO: Make this good in our scheduler.
 */

//static spinlock s_lock;
// This should be assumed to be called with the current thread's context saved.
// It does NOT do that on it's own.
spinlock Core_SchedulerLock;
static bool suspendSched = false;
static _Atomic(size_t) nSuspended = 0;
__attribute__((no_instrument_function)) void Core_Schedule()
{
	// NOTE: Do not remove.
	if (suspendSched)
		nSuspended++;
	if (suspendSched)
	{
		irql oldIrql = Core_RaiseIrql(IRQL_MASKED);
		for (volatile bool b = suspendSched; b; )
#if defined(__x86_64__) || defined(__i686__)
			asm volatile ("hlt")
#endif
			;
		Core_LowerIrql(oldIrql);
	}
	getSchedulerTicks++;
	if (!getCurrentThread)
		goto schedule;
	getCurrentThread->lastRunTick = getSchedulerTicks;
	schedule:
	if (getCurrentThread)
	{
		getCurrentThread->quantum = 0;
		thread_priority_list* list = priorityList(getCurrentThread->priority);
		if (getCurrentThread->flags & THREAD_FLAGS_PRIORITY_RAISED)
		{
			CoreH_ThreadListRemove(&list->list, getCurrentThread->snode);
			getCurrentThread->priority--;
			getCurrentThread->flags &= ~THREAD_FLAGS_PRIORITY_RAISED;
			CoreH_ThreadListAppend(&(priorityList(getCurrentThread->priority)->list), getCurrentThread->snode);
		}
		if (getCurrentThread->status != THREAD_STATUS_BLOCKED)
			getCurrentThread->status = THREAD_STATUS_READY;
	}
	// thread* oldCurThread = getCurrentThread;
	// getCurrentThread = nullptr;
	// (void)Core_SpinlockAcquireExplicit(&Core_SchedulerLock, IRQL_DISPATCH, true);
	// (void)Core_SpinlockAcquireExplicit(&CoreS_GetCPULocalPtr()->schedulerLock, IRQL_DISPATCH, true);
thread* chosenThread = nullptr;
timer_tick CoreS_GetNativeTimerTick();
// timer_tick start = CoreS_GetNativeTimerTick();
	if (obos_expect(getCurrentThread != nullptr, true))
		chosenThread = getCurrentThread->snode->next ? getCurrentThread->snode->next->data : nullptr;
	if (chosenThread)
		goto switch_thread;

	if (!CoreS_GetCPULocalPtr()->currentPriorityList)
		CoreS_GetCPULocalPtr()->currentPriorityList = priorityList(THREAD_PRIORITY_MAX_VALUE);

	get_next_list:
	(void)0;
	// Go to the next priority list.
	thread_priority next_priority = CoreS_GetCPULocalPtr()->currentPriorityList->priority-1;
	if (next_priority < 0)
		next_priority = THREAD_PRIORITY_MAX_VALUE;

	CoreS_GetCPULocalPtr()->currentPriorityList = priorityList(next_priority);
	if (!CoreS_GetCPULocalPtr()->currentPriorityList->list.head)
		goto get_next_list;

	chosenThread = CoreS_GetCPULocalPtr()->currentPriorityList->list.head->data;

switch_thread:
	(void)0;
	// timer_tick end = CoreS_GetNativeTimerTick();
	// CoreS_GetCPULocalPtr()->last_sched_algorithm_time = end-start;
	OBOS_ASSERT(chosenThread);
	// if (chosenThread == getCurrentThread)
	// 	return; // We might as well save some time and return.
	// Or maybe not.....
	if (chosenThread != getCurrentThread)
		OBOS_ASSERT(chosenThread->status != THREAD_STATUS_RUNNING);
	chosenThread->status = THREAD_STATUS_RUNNING;
	chosenThread->masterCPU = CoreS_GetCPULocalPtr();
	chosenThread->quantum = 0 /* should be zero, but reset it anyway */;
	// Core_SpinlockRelease(&Core_SchedulerLock, IRQL_DISPATCH);
	// Core_SpinlockRelease(&CoreS_GetCPULocalPtr()->schedulerLock, IRQL_DISPATCH);
	getCurrentThread = chosenThread;
	if (chosenThread->proc)
		CoreS_GetCPULocalPtr()->currentContext = chosenThread->proc->ctx;
	CoreS_GetCPULocalPtr()->currentKernelStack = chosenThread->kernelStack;
	CoreS_SetKernelStack(chosenThread->kernelStack);
	// for (volatile bool b = (chosenThread->tid == 7); b; )
	// 	asm volatile ("":"=r"(b) :"r"(b) :"memory");
	CoreS_SwitchToThreadContext(&chosenThread->context);
}
struct irq* Core_SchedulerIRQ;
uint64_t Core_SchedulerTimerFrequency = 1000;
void Core_Yield()
{
	if (getCurrentThread && getCurrentThread->kill && (getCurrentThread->yield_count_since_kill++ != 1 /* give the code a chance to remove itself from existance */))
		Core_ExitCurrentThread();
	irql oldIrql = IRQL_INVALID;
	if (Core_GetIrql() <= IRQL_DISPATCH)
	{
		oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
		OBOS_ASSERT(!(oldIrql & ~0xf));
	}
	if (getCurrentThread)
	{
		bool canRunCurrentThread = threadCanRunThread(getCurrentThread);
		++getCurrentThread->total_quantums;
		if (++getCurrentThread->quantum < Core_ThreadPriorityToQuantum[getCurrentThread->priority] && canRunCurrentThread)
		{
			if (oldIrql != IRQL_INVALID)
			{
				OBOS_ASSERT(!(oldIrql & ~0xf));
				Core_LowerIrql(oldIrql);
			}
			return; // No rescheduling needed, as the thread's quantum isn't finished yet.
		}
		CoreS_SaveRegisterContextAndYield(&getCurrentThread->context);
		if (oldIrql != IRQL_INVALID)
		{
			OBOS_ASSERT(!(oldIrql & ~0xf));
			Core_LowerIrql(oldIrql);
		}
		return;
	}
	CoreS_CallFunctionOnStack((uintptr_t(*)(uintptr_t))(void*)Core_Schedule, 0);
	if (oldIrql != IRQL_INVALID)
	{
		OBOS_ASSERT(!(oldIrql & ~0xf));
		Core_LowerIrql(oldIrql);
	}
}

void Core_SuspendScheduler(bool suspended)
{
	suspendSched = suspended;
	nSuspended = 0;
}
void Core_WaitForSchedulerSuspend()
{
	while (suspendSched && nSuspended < (Core_CpuCount - 1))
		OBOSS_SpinlockHint();
}
