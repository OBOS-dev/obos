/*
	oboskrnl/scheduler/thread.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

static uint64_t s_nextTID = 1;
cpu_local* Core_CpuInfo;
thread_affinity Core_DefaultThreadAffinity = 1;
size_t Core_CpuCount;
static void free_thr(thread* thr)
{
	OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, thr, sizeof(*thr));
}
static void free_node(thread_node* node)
{
	OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, node, sizeof(*node));
}
thread* CoreH_ThreadAllocate(obos_status* status)
{
	thread* thr = nullptr;
	if (!OBOS_NonPagedPoolAllocator)
		thr = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread), status);
	else
		thr = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(thread), status);
	if (thr)
		thr->free = free_thr;
	return thr;
}
obos_status CoreH_ThreadInitialize(thread* thr, thread_priority priority, thread_affinity affinity, const thread_ctx* ctx)
{
	if (!thr || !ctx || priority < 0 || priority > THREAD_PRIORITY_MAX_VALUE || !affinity)
		return OBOS_STATUS_INVALID_ARGUMENT;
	thr->priority = priority;
	thr->status = THREAD_STATUS_READY;
	thr->tid = s_nextTID++;
	thr->context = *ctx;
	thr->affinity = affinity;
	thr->masterCPU = nullptr;
	thr->quantum = 0;
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadReady(thread* thr)
{
	if (!OBOS_KernelAllocator)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	thread_node* node = (thread_node*)OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_node), nullptr);
	node->free = free_node;
	obos_status status = CoreH_ThreadReadyNode(thr, node);
	if (status != OBOS_STATUS_SUCCESS)
		free_node(node);
	return status;
}
obos_status CoreH_ThreadReadyNode(thread* thr, thread_node* node)
{
	// Find the processor with the least threads using the current thread's priority.
	// Then, add the node to that list.
	if (!Core_CpuInfo)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!thr || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->priority < 0 || thr->priority > THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->masterCPU)
		return OBOS_STATUS_SUCCESS;
	cpu_local* cpuFound = nullptr;
	for (size_t cpui = 0; cpui < Core_CpuCount; cpui++)
	{
		cpu_local* cpu = &Core_CpuInfo[cpui];
		if (!(thr->affinity & CoreH_CPUIdToAffinity(cpu->id)))
			continue;
		if (!cpuFound || cpu->priorityLists[thr->priority].list.nNodes < cpuFound->priorityLists[thr->priority].list.nNodes)
			cpuFound = cpu;
	}
	if (!cpuFound)
		return OBOS_STATUS_INVALID_AFFINITY;
	irql oldIrql = Core_SpinlockAcquire(&Core_SchedulerLock);
	irql oldIrql2 = Core_SpinlockAcquire(&cpuFound->schedulerLock);
	node->data = thr;
	thr->snode = node;
	thr->masterCPU = cpuFound;
	thr->status = THREAD_STATUS_READY;
	thread_list* priorityList = &cpuFound->priorityLists[thr->priority].list;
	Core_ReadyThreadCount++;
	obos_status status = CoreH_ThreadListAppend(priorityList, node);
	Core_SpinlockRelease(&thr->masterCPU->schedulerLock, oldIrql2);
	Core_SpinlockRelease(&Core_SchedulerLock, oldIrql);
	return status;
}
obos_status CoreH_ThreadBlock(thread* thr, bool canYield)
{
	if (!thr)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!thr->masterCPU || thr->priority < 0 || thr->priority > THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->status == THREAD_STATUS_BLOCKED)
		return OBOS_STATUS_SUCCESS;
	irql oldIrql2 = Core_SpinlockAcquire(&Core_SchedulerLock);
	irql oldIrql = Core_SpinlockAcquire(&thr->masterCPU->schedulerLock);
	thread_node* node = thr->snode;
	CoreH_ThreadListRemove(&thr->masterCPU->priorityLists[thr->priority].list, node);
	thr->flags &= ~THREAD_FLAGS_PRIORITY_RAISED;
	thr->status = THREAD_STATUS_BLOCKED;
	thr->quantum = 0;
	// TODO: Send an IPI of some sort to make sure the other CPU yields if this current thread is running.
	Core_ReadyThreadCount--;
	Core_SpinlockRelease(&thr->masterCPU->schedulerLock, oldIrql);
	Core_SpinlockRelease(&Core_SchedulerLock, oldIrql2);
	thr->masterCPU = nullptr;
	if (thr == Core_GetCurrentThread() && canYield)
		Core_Yield();
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadBoostPriority(thread* thr)
{
	if (!thr)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->priority < 0 || thr->priority > THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->flags & THREAD_FLAGS_DIED)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!thr->masterCPU && thr->status != THREAD_STATUS_BLOCKED)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (thr->flags & THREAD_FLAGS_PRIORITY_RAISED || thr->priority == THREAD_PRIORITY_MAX_VALUE)
		return OBOS_STATUS_SUCCESS;
	irql oldIrql2 = Core_SpinlockAcquire(&Core_SchedulerLock);
	irql oldIrql = thr->masterCPU ? Core_SpinlockAcquire(&thr->masterCPU->schedulerLock) : IRQL_INVALID;
	if (thr->masterCPU)
	{
		CoreH_ThreadListRemove(&thr->masterCPU->priorityLists[thr->priority].list, thr->snode);
		CoreH_ThreadListAppend(&thr->masterCPU->priorityLists[thr->priority+1].list, thr->snode);
	}
	thr->flags |= THREAD_FLAGS_PRIORITY_RAISED;
	thr->priority++;
	if (thr->masterCPU)
		Core_SpinlockRelease(&thr->masterCPU->schedulerLock, oldIrql);
	Core_SpinlockRelease(&Core_SchedulerLock, oldIrql2);
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadListAppend(thread_list* list, thread_node* node)
{
	if (!list || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!node->data)
		return OBOS_STATUS_INVALID_ARGUMENT;
	irql oldIrql = Core_SpinlockAcquire(&list->lock);
	if (list->tail)
		list->tail->next = node;
	if(!list->head)
		list->head = node;
	node->prev = list->tail;
	list->tail = node;
	list->nNodes++;
	if (Core_SpinlockRelease(&list->lock, oldIrql) != OBOS_STATUS_SUCCESS)
	{
		Core_LowerIrql(oldIrql);
		Core_SpinlockForcedRelease(&list->lock);
	}
	return OBOS_STATUS_SUCCESS;
}
obos_status CoreH_ThreadListRemove(thread_list* list, thread_node* node)
{
	if (!list || !node)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!node->data)
		return OBOS_STATUS_INVALID_ARGUMENT;
	irql oldIrql = Core_SpinlockAcquire(&list->lock);
#ifdef OBOS_DEBUG
	thread_node* cur = list->head;
	for (; cur;)
	{
		if (cur == node)
			break;
		cur = cur->next;
	}
	OBOS_ASSERT(cur);
#endif
	if (node->next)
		node->next->prev = node->prev;
	if (node->prev)
		node->prev->next = node->next;
	if (list->head == node)
		list->head = node->next;
	if (list->tail == node)
		list->tail = node->prev;
	list->nNodes--;
	node->next = nullptr;
	node->prev = nullptr;
	if (Core_SpinlockRelease(&list->lock, oldIrql) != OBOS_STATUS_SUCCESS)
	{
		Core_LowerIrql(oldIrql);
		Core_SpinlockForcedRelease(&list->lock);
	}
	return OBOS_STATUS_SUCCESS;
}
thread_affinity CoreH_CPUIdToAffinity(uint32_t cpuId)
{
	return ((thread_affinity)1 << cpuId);
}

OBOS_NORETURN OBOS_PAGEABLE_FUNCTION static uintptr_t ExitCurrentThread(uintptr_t unused)
{
	OBOS_UNUSED(unused);
	thread* currentThread = Core_GetCurrentThread();
	// Block (unready) the current thread so it can no longer be run.
	thread_node* node = currentThread->snode;
	CoreH_ThreadBlock(currentThread, false);
	currentThread->flags |= THREAD_FLAGS_DIED;
	CoreS_FreeThreadContext(&currentThread->context);
	if (node->free)
		node->free(node);
	if (!currentThread->references && currentThread->free)
		currentThread->free(currentThread);
	CoreS_GetCPULocalPtr()->currentThread = nullptr;
	Core_Yield();
	OBOS_UNREACHABLE;
}
OBOS_NORETURN OBOS_PAGEABLE_FUNCTION void Core_ExitCurrentThread()
{
	(void)Core_RaiseIrqlNoThread(IRQL_DISPATCH);
	CoreS_CallFunctionOnStack(ExitCurrentThread, 0);
	OBOS_UNREACHABLE;
}
