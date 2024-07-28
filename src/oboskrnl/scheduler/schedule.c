/*
	oboskrnl/scheduler/schedule.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>
#include <scheduler/process.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#define getCurrentThread (CoreS_GetCPULocalPtr()->currentThread)
#define getIdleThread (CoreS_GetCPULocalPtr()->idleThread)
#define getSchedulerTicks (CoreS_GetCPULocalPtr()->schedulerTicks)
#define getPriorityLists (CoreS_GetCPULocalPtr()->priorityLists)
#define priorityList(priority) ((priority <= THREAD_PRIORITY_MAX_VALUE) ? &getPriorityLists[priority] : nullptr)
#define verifyAffinity(thr, cpuId) (thr->affinity & CoreH_CPUIdToAffinity(cpuId))
#define threadCanRunThread(thr) ((thr->status == THREAD_STATUS_RUNNING || thr->status == THREAD_STATUS_READY) && verifyAffinity(thr, CoreS_GetCPULocalPtr()->id))

size_t Core_ReadyThreadCount;
const uint8_t Core_ThreadPriorityToQuantum[THREAD_PRIORITY_MAX_VALUE+1] = {
	2, // THREAD_PRIORITY_IDLE
	4, // THREAD_PRIORITY_LOW
	8, // THREAD_PRIORITY_NORMAL
	12, // THREAD_PRIORITY_HIGH
	12, // THREAD_PRIORITY_URGENT
};

thread* Core_GetCurrentThread() { if (!CoreS_GetCPULocalPtr()) return nullptr; return getCurrentThread; }

/*
 * The scheduler is the thing that chooses a thread to run.
 * Many hobby OS developers think that's all it should do.
 * That is wrong, or rather that's not all it should do, in my opinion.
 * It is also a priority manager, it must make sure no threads starve by temporarily raising their priority.
 * The scheduler must do load balancing.
*/

// Returns false if the quantum is done, otherwise true if the action was completed
static bool ThreadStarvationPrevention(thread_priority_list* list, thread_priority priority)
{
	size_t i = 0;
	if (++list->noStarvationQuantum < Core_ThreadPriorityToQuantum[THREAD_PRIORITY_MAX_VALUE - priority])
		return false;
	// The last (list->nNodes / 4) threads in a priority list will be starving usually because they are at the end of the list, and the scheduler starts looking from the front.
	for (thread_node* thrN = list->list.tail; thrN && i < (list->list.nNodes / 4); )
	{
		if (thrN->data == CoreS_GetCPULocalPtr()->idleThread)
		{
			thrN = thrN->prev;
			continue;
		}
		if (thrN->data->status == THREAD_STATUS_RUNNING)
		{
			thrN = thrN->prev;
			continue;
		}
		OBOS_ASSERT(thrN->data->status == THREAD_STATUS_READY);
		if (thrN->data->flags & THREAD_FLAGS_PRIORITY_RAISED)
		{
			thrN = thrN->prev;
			continue;
		}
		thread_node* next = thrN->prev;
		CoreH_ThreadListRemove(&list->list, thrN);
		CoreH_ThreadListAppend(&(priorityList(priority + 1)->list), thrN);
		thrN->data->flags |= THREAD_FLAGS_PRIORITY_RAISED;
		thrN->data->priority++;
		thrN = next;
	}
	return true;
}
static void WorkStealing(thread_priority_list* list, thread_priority priority)
{
	OBOS_ASSERT(priority <= THREAD_PRIORITY_MAX_VALUE);
	OBOS_ASSERT(list);
	// Compare the current list's node count to that of other cores, and if it is less than the node count of one quarter of cores, then steal some work from some of the cores.
	size_t nCoresWithMoreNodes = 0;
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
		if (cpu == CoreS_GetCPULocalPtr())
			continue;
		if (cpu->priorityLists[priority].list.nNodes > list->list.nNodes)
			nCoresWithMoreNodes++;
	}
	if (nCoresWithMoreNodes < (Core_CpuCount / 4))
		return; // The work count for this priority list is fine.
	// Try to steal some work...
	// NOTE: There could be some race conditions with this.
	// TODO: Fix.
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
		if (cpu == CoreS_GetCPULocalPtr())
			continue;
		if (cpu->priorityLists[priority].list.nNodes > list->list.nNodes)
		{
			size_t targetNodeCount = cpu->priorityLists[priority].list.nNodes;
			size_t ourNodeCount = list->list.nNodes;
			size_t j = 0;
			(void)Core_SpinlockAcquireExplicit(&cpu->schedulerLock, IRQL_DISPATCH, true);
			// Steal some work from the target CPU.
			for (thread_node* thrN = cpu->priorityLists[priority].list.head; thrN && j < ((targetNodeCount - ourNodeCount) / nCoresWithMoreNodes + 1); j++)
			{
				if (thrN->data->status != THREAD_STATUS_READY)
				{
					thrN = thrN->next;
					continue;
				}
				if (thrN->data->flags & THREAD_FLAGS_PRIORITY_RAISED)
				{
					thrN = thrN->next;
					continue;
				}
				if (!verifyAffinity(thrN->data, CoreS_GetCPULocalPtr()->id))
				{
					thrN = thrN->next;
					continue;
				}
				// Steal this thread.
				thread_node* next = thrN->next;
				CoreH_ThreadListRemove(&cpu->priorityLists[priority].list, thrN);
				CoreH_ThreadListAppend(&(priorityList(priority)->list), thrN);
				thrN->data->masterCPU = CoreS_GetCPULocalPtr();
				thrN = next;
			}
			Core_SpinlockRelease(&cpu->schedulerLock, IRQL_DISPATCH);
		}
	}
}

//static spinlock s_lock;
// This should be assumed to be called with the current thread's context saved.
// It does NOT do that on it's own.
spinlock Core_SchedulerLock;
void Core_Schedule()
{
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
	}
	// thread* oldCurThread = getCurrentThread;
	// getCurrentThread = nullptr;
	(void)Core_SpinlockAcquireExplicit(&Core_SchedulerLock, IRQL_DISPATCH, true);
	(void)Core_SpinlockAcquireExplicit(&CoreS_GetCPULocalPtr()->schedulerLock, IRQL_DISPATCH, true);
	// Thread starvation prevention and work stealing.
	// The amount of priority lists with a finished (starvation) quantum.
	size_t nPriorityListsFQuantum = 0;
	for (thread_priority priority = THREAD_PRIORITY_IDLE; priority <= THREAD_PRIORITY_MAX_VALUE; priority++)
	{
		thread_priority_list* list = priorityList(priority);
		if (priority < THREAD_PRIORITY_MAX_VALUE)
			nPriorityListsFQuantum += (size_t)!(ThreadStarvationPrevention(list, priority));
		WorkStealing(list, priority);
	}
	if (nPriorityListsFQuantum == THREAD_PRIORITY_MAX_VALUE)
	{
		for (thread_priority priority = THREAD_PRIORITY_IDLE; priority <= THREAD_PRIORITY_MAX_VALUE-1; priority++)
		{
			thread_priority_list* list = priorityList(priority);
			list->noStarvationQuantum = 0;
		}
	}
	thread* chosenThread = nullptr;
	if (!CoreS_GetCPULocalPtr()->currentPriorityList)
		CoreS_GetCPULocalPtr()->currentPriorityList = &CoreS_GetCPULocalPtr()->priorityLists[THREAD_PRIORITY_MAX_VALUE];
	if (++CoreS_GetCPULocalPtr()->currentPriorityList->quantum >= Core_ThreadPriorityToQuantum[CoreS_GetCPULocalPtr()->currentPriorityList->priority])
	{
		CoreS_GetCPULocalPtr()->currentPriorityList->quantum = 0;
		thread_priority nextPriority = CoreS_GetCPULocalPtr()->currentPriorityList->priority - 1;
		if (nextPriority < 0)
			nextPriority = THREAD_PRIORITY_MAX_VALUE;
		CoreS_GetCPULocalPtr()->currentPriorityList = priorityList(nextPriority);
		
	}
	thread_priority_list* list = CoreS_GetCPULocalPtr()->currentPriorityList;
	find_list:
	OBOS_ASSERT(list);
	while (!list->list.head)
	{
		thread_priority nextPriority = list->priority - 1;
		if (nextPriority < 0)
		{
			list = nullptr;
			chosenThread = CoreS_GetCPULocalPtr()->idleThread;
			if (!chosenThread)
				OBOS_Panic(OBOS_PANIC_SCHEDULER_ERROR, "Error in %s while rescheduling CPU %d: Could not find an appropriate idle thread when all thread lists have exhausted.\n", __func__, CoreS_GetCPULocalPtr()->id);
			break;
		}
		OBOS_ASSERT(nextPriority <= THREAD_PRIORITY_MAX_VALUE);
		list = priorityList(nextPriority);
	}
	if (!list)
		goto switch_thread;
	thread_node* node = list->list.head;
	while (node && node->data->status == THREAD_STATUS_RUNNING)
		node = node->next;
	if (!node)
	{
		thread_priority nextPriority = list->priority - 1;
		if (nextPriority < 0)
		{
			list = nullptr;
			chosenThread = CoreS_GetCPULocalPtr()->idleThread;
			if (!chosenThread)
				OBOS_Panic(OBOS_PANIC_SCHEDULER_ERROR, "Error in %s while rescheduling CPU %d: Could not find an appropriate idle thread when all thread lists have exhausted.\n", __func__, CoreS_GetCPULocalPtr()->id);
			goto switch_thread;
		}
		list = priorityList(nextPriority);
		goto find_list;
	}

	chosenThread = node->data;
switch_thread:
	OBOS_ASSERT(chosenThread);
	// if (chosenThread == getCurrentThread)
	// 	return; // We might as well save some time and return.
	// Or maybe not.....
	if (chosenThread != getCurrentThread)
		OBOS_ASSERT(chosenThread->status != THREAD_STATUS_RUNNING);
	chosenThread->status = THREAD_STATUS_RUNNING;
	chosenThread->masterCPU = CoreS_GetCPULocalPtr();
	chosenThread->quantum = 0 /* should be zero, but reset it anyway */;
	if (getCurrentThread)
		getCurrentThread->status = THREAD_STATUS_READY;
	Core_SpinlockRelease(&Core_SchedulerLock, IRQL_DISPATCH);
	Core_SpinlockRelease(&CoreS_GetCPULocalPtr()->schedulerLock, IRQL_DISPATCH);
	getCurrentThread = chosenThread;
	if (chosenThread->proc)
		CoreS_GetCPULocalPtr()->currentContext = chosenThread->proc->ctx;
	CoreS_SwitchToThreadContext(&chosenThread->context);
}
struct irq* Core_SchedulerIRQ;
uint64_t Core_SchedulerTimerFrequency = 1000;
void Core_Yield()
{
	irql oldIrql = IRQL_INVALID;
	if (Core_GetIrql() <= IRQL_DISPATCH)
	{
		oldIrql = Core_RaiseIrqlNoThread(IRQL_DISPATCH);
		OBOS_ASSERT(!(oldIrql & ~0xf));
	}
	if (getCurrentThread)
	{
		bool canRunCurrentThread = threadCanRunThread(getCurrentThread);
		if (++getCurrentThread->quantum < Core_ThreadPriorityToQuantum[getCurrentThread->priority] && canRunCurrentThread)
		{
			if (oldIrql != IRQL_INVALID)
			{
				OBOS_ASSERT(!(oldIrql & ~0xf));
				Core_LowerIrqlNoThread(oldIrql);
			}
			return; // No rescheduling needed, as the thread's quantum isn't finished yet.
		}
		CoreS_SaveRegisterContextAndYield(&getCurrentThread->context);
		if (oldIrql != IRQL_INVALID)
		{
			OBOS_ASSERT(!(oldIrql & ~0xf));
			Core_LowerIrqlNoThread(oldIrql);
		}
		return;
	}
	CoreS_CallFunctionOnStack((uintptr_t(*)(uintptr_t))Core_Schedule, 0);
	if (oldIrql != IRQL_INVALID)
	{
		OBOS_ASSERT(!(oldIrql & ~0xf));
		Core_LowerIrqlNoThread(oldIrql);
	}
}