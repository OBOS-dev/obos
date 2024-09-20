/*
 * oboskrnl/scheduler/process.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>

#include <allocators/base.h>

uint64_t Core_NextPID = 1;
static OBOS_PAGEABLE_FUNCTION void free_node(thread_node* n)
{
	OBOS_KernelAllocator->Free(OBOS_KernelAllocator, n, sizeof(*n));
}
OBOS_PAGEABLE_FUNCTION process* Core_ProcessAllocate(obos_status* status) 
{
	if (!OBOS_KernelAllocator)
	{
		if (status)
			*status = OBOS_STATUS_INVALID_INIT_PHASE;
		return nullptr;
	}
	if (!OBOS_NonPagedPoolAllocator)
		return OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(process), status);
	return OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(process), status);
}
OBOS_PAGEABLE_FUNCTION obos_status Core_ProcessStart(process* proc, thread* mainThread)
{
	if (!OBOS_KernelAllocator)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!proc || !mainThread)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!mainThread->affinity || mainThread->masterCPU || mainThread->proc)
		return OBOS_STATUS_INVALID_ARGUMENT;
	proc->pid = Core_NextPID++;
	obos_status status = OBOS_STATUS_SUCCESS;
	thread_node* node = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_node), &status);
	if (obos_is_error(status))
		return status;
	node->free = free_node;
	node->data = mainThread;
	CoreH_ThreadListAppend(&proc->threads, node);
	mainThread->proc = proc;
	return CoreH_ThreadReady(mainThread);
}
OBOS_PAGEABLE_FUNCTION obos_status Core_ProcessAppendThread(process* proc, thread* thread)
{
	if (!OBOS_KernelAllocator)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!proc || !thread)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!thread->affinity || thread->proc)
		return OBOS_STATUS_INVALID_ARGUMENT;
	obos_status status = OBOS_STATUS_SUCCESS;
	thread_node* node = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_node), &status);
	if (obos_is_error(status))
		return status;
	node->free = free_node;
	node->data = thread;
	thread->pnode = node;
	CoreH_ThreadListAppend(&proc->threads, node);
	thread->proc = proc;
	return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status Core_ProcessTerminate(process* proc, bool forced)
{
	OBOS_UNUSED(proc);
	OBOS_UNUSED(forced);
	return OBOS_STATUS_UNIMPLEMENTED;
}