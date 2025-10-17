/*
 * oboskrnl/scheduler/process.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <handle.h>
#include <klog.h>
#include <signal.h>

#include <vfs/alloc.h>
#include <vfs/mount.h>

#include <irq/irql.h>

#include <scheduler/thread_context_info.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <locks/spinlock.h>
#include <locks/wait.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <allocators/base.h>

#include <utils/tree.h>

LIST_GENERATE(process_list, process, node);

static int pgrp_cmp(const process_group* lhs, const process_group* rhs)
{ return lhs->pgid < rhs->pgid ? -1 : (lhs->pgid == rhs->pgid ? 0 : 1); }
RB_GENERATE(process_group_tree, process_group, rb_node, pgrp_cmp);

process_group_tree Core_ProcessGroups;
mutex Core_ProcessGroupTreeLock;

process* OBOS_KernelProcess;
uint32_t Core_NextPID = 0;
static OBOS_PAGEABLE_FUNCTION void free_node(thread_node* n)
{
	Free(OBOS_KernelAllocator, n, sizeof(*n));
}
OBOS_PAGEABLE_FUNCTION process* Core_ProcessAllocate(obos_status* status) 
{
	if (!OBOS_KernelAllocator)
	{
		if (status)
			*status = OBOS_STATUS_INVALID_INIT_PHASE;
		return nullptr;
	}
	if (obos_expect(!OBOS_NonPagedPoolAllocator && Core_NextPID > 0, false))
	{
		if (status)
			*status = OBOS_STATUS_INVALID_INIT_PHASE;
		return nullptr;
	}
	if (!OBOS_NonPagedPoolAllocator)
		return ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(process), status);
	return ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(process), status);
}
OBOS_PAGEABLE_FUNCTION obos_status Core_ProcessStart(process* proc, thread* mainThread)
{
	if (!OBOS_KernelAllocator)
		return OBOS_STATUS_INVALID_INIT_PHASE;
	if (!proc)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (mainThread)
		if (!mainThread->affinity || mainThread->masterCPU || mainThread->proc)
			return OBOS_STATUS_INVALID_ARGUMENT;
	proc->pid = Core_NextPID++;
	proc->refcount++;

	proc->parent = Core_GetCurrentThread()->proc;
	irql oldIrql = Core_SpinlockAcquire(&proc->parent->children_lock);
	proc->parent->refcount++;
	if (proc->parent->children.tail)
		proc->parent->children.tail->next = proc;
	if (!proc->parent->children.head)
		proc->parent->children.head = proc;
	proc->prev = proc->parent->children.tail;
	proc->parent->children.tail = proc;
	proc->parent->children.nChildren++;
	proc->refcount++;
	proc->pgrp = proc->parent->pgrp;
	proc->waiting_threads = WAITABLE_HEADER_INITIALIZE(false, true);
	Core_SpinlockRelease(&proc->parent->children_lock, oldIrql);
	// if (proc->controlling_tty && proc->controlling_tty->fg_job == proc->parent)
	// 	proc->controlling_tty->fg_job = proc; // we are the new foreground job.

	if (!proc->parent->cwd)
	{
		proc->cwd = Vfs_Root;
		proc->cwd_str = memcpy(Vfs_Malloc(2), "/", 2);
	}
	else
	{
		proc->cwd = proc->parent->cwd;
		size_t len = strlen(proc->parent->cwd_str);
		proc->cwd_str = memcpy(Vfs_Malloc(len+1), proc->parent->cwd_str, len+1);
	}

	// If we fork using Sys_ProcessStart, then the handle table is conviniently
	// already setup.
	if (!proc->handles.size)
		OBOS_InitializeHandleTable(&proc->handles);
	if (!mainThread)
		return OBOS_STATUS_SUCCESS;
	obos_status status = OBOS_STATUS_SUCCESS;
	thread_node* node = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_node), &status);
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
	thread_node* node = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(thread_node), &status);
	if (obos_is_error(status))
		return status;
	node->free = free_node;
	node->data = thread;
	thread->pnode = node;
	CoreH_ThreadListAppend(&proc->threads, node);
	thread->proc = proc;
	thread->pnode = node;
	return OBOS_STATUS_SUCCESS;
}

uintptr_t ExitCurrentProcess(uintptr_t unused)
{
	OBOS_UNUSED(unused);
	process* proc = Core_GetCurrentThread()->proc;
	// Disown all children.
	for (process* child = proc->children.head; child; )
	{
		process* next = child->next;
		if (child->next)
			child->next->prev = child->prev;
		// child->prev will always be null
		child->next = nullptr;

		if (!(--child->refcount))
		{
			Free(OBOS_NonPagedPoolAllocator, child, sizeof(*child));
			child = next;
			continue;
		}

		// The kernel is the child's adoptive parent
		child->parent = OBOS_KernelProcess;
		irql oldIrql = Core_SpinlockAcquire(&child->parent->children_lock);
		if (child->parent->children.tail)
			child->parent->children.tail->next = child;
		if (!child->parent->children.head)
			child->parent->children.head = child;
		child->prev = child->parent->children.tail;
		child->parent->children.tail = child;
		child->parent->children.nChildren++;
		Core_SpinlockRelease(&child->parent->children_lock, oldIrql);
		child->refcount++;

		child = next;
	}
	proc->children.head = nullptr;
	proc->children.tail = nullptr;
	proc->children.nChildren = 0;

	if (proc->parent == OBOS_KernelProcess)
	{
		// Our parent is the kernel process, since we won't get reaped,
		// remove ourselves from the children list here.
		irql oldIrql = Core_SpinlockAcquire(&OBOS_KernelProcess->children_lock);
		if (proc->next)
			proc->next->prev = proc->prev;
		if (proc->next)
			proc->next->prev = proc->prev;
		if (OBOS_KernelProcess->children.head == proc)
			OBOS_KernelProcess->children.head = proc->next;
		if (OBOS_KernelProcess->children.tail == proc)
			OBOS_KernelProcess->children.tail = proc->prev;
		OBOS_KernelProcess->children.nChildren--;
		Core_SpinlockRelease(&OBOS_KernelProcess->children_lock, oldIrql);
		proc->refcount--;
	}

	// Close all handles.
	for (uint32_t i = 0; i < (uint32_t)proc->handles.size; i++)
	{
		handle hnd = i;
		hnd |= (proc->handles.arr[i].type << HANDLE_TYPE_SHIFT);
		if (proc->handles.arr[i].un.as_int)
			Sys_HandleClose(hnd);
	}

	// Free all memory (TODO: Better strategy?)
	page_range* rng = nullptr;
	page_range* next = nullptr;

	for ((rng) = RB_MIN(page_tree, &proc->ctx->pages); (rng) != nullptr; )
	{
		OBOS_ENSURE(rng);
		next = RB_NEXT(page_tree, &ctx->pages, rng);
		uintptr_t virt = rng->virt;
		if (rng->hasGuardPage)
			virt += (rng->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
		uintptr_t limit = rng->virt+rng->size;
		Mm_VirtualMemoryFree(proc->ctx, (void*)virt, limit-virt);
		rng = next;
	}

	MmS_FreePageTable(proc->ctx->pt);
	Mm_Allocator->Free(Mm_Allocator, proc->ctx, sizeof(context));

	thread* ready = nullptr;
	thread* running = nullptr;
	thread* blocked = nullptr;
	for (thread_node* node = proc->parent->threads.head; node; )
	{
		thread* const thr = node->data;
		node = node->next;

		if (thr->status == THREAD_STATUS_READY)
			ready = thr;
		if (thr->status == THREAD_STATUS_BLOCKED && ~thr->flags & THREAD_FLAGS_DIED)
			blocked = thr;

		if (thr->status == THREAD_STATUS_RUNNING)
		{
			running = thr;
			break;
		}
	}

	if (running)
		OBOS_Kill(Core_GetCurrentThread(), running, SIGCHLD);
	else if (ready)
		OBOS_Kill(Core_GetCurrentThread(), ready, SIGCHLD);
	else if (blocked)
		OBOS_Kill(Core_GetCurrentThread(), blocked, SIGCHLD);

	CoreH_SignalWaitingThreads(WAITABLE_OBJECT(*proc), true, false);

	proc->dead = true;
	
	if (!(--proc->refcount))
		Free(OBOS_NonPagedPoolAllocator, proc, sizeof(*proc));

	Core_GetCurrentThread()->userStack = nullptr;
	Core_GetCurrentThread()->proc = nullptr;
	Core_ExitCurrentThread();
}

OBOS_NORETURN void Core_ExitCurrentProcess(uint32_t code)
{
	process* proc = Core_GetCurrentThread()->proc;
	if (proc->pid == 0)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Attempt to exit current process in the kernel process\n");

	proc->exitCode = code;

	Core_ExitProcessGroup();

	// Kill all threads.
	for (thread_node* node = proc->threads.head; node; )
	{
		thread* const thr = node->data;
		node = node->next;
		if (thr == Core_GetCurrentThread())
			continue;

		thr->references++;

		// OBOS_Kill(Core_GetCurrentThread(), thr, SIGKILL);
		thr->kill = true;
		thr->interrupted = true;
		thr->signalInterrupted = true;
		CoreH_ThreadReady(thr);

		// NOTE: This loop should not take too long.
		while (~thr->flags & THREAD_FLAGS_DIED)
			OBOSS_SpinlockHint();

		if (!(--thr->references) && thr->free)
			thr->free(thr);
	}
	irql oldIrql = Core_GetIrql() < IRQL_DISPATCH ? Core_RaiseIrql(IRQL_DISPATCH) : 0;
	(void)oldIrql;
	ExitCurrentProcess(code);
	while (1)
		;
}

void Core_ExitProcessGroup()
{
	process* proc = Core_GetCurrentThread()->proc;
	if (!proc->pgrp)
		return;
	process_group* pgrp = proc->pgrp;
	Core_MutexAcquire(&pgrp->lock);
	LIST_REMOVE(process_list, &pgrp->processes, proc);
	if (pgrp->leader == proc)
		pgrp->leader = nullptr;
	if (!LIST_GET_NODE_COUNT(process_list, &pgrp->processes))
	{
		// process group is ded :/
		Core_MutexAcquire(&Core_ProcessGroupTreeLock);
		RB_REMOVE(process_group_tree, &Core_ProcessGroups, pgrp);
		Core_MutexRelease(&Core_ProcessGroupTreeLock);
	}
	Core_MutexRelease(&pgrp->lock);
}

obos_status Core_SetProcessGroup(process* proc, uint32_t pgid)
{
	if (!proc)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!pgid)
		pgid = proc->pid;

	// TODO: Check session IDs
	if (proc != Core_GetCurrentThread()->proc && proc->parent != Core_GetCurrentThread()->proc)
		return OBOS_STATUS_NOT_FOUND; // ESRCH?

	if (proc->pgrp && proc->pgrp->leader == proc)
		return proc->pgrp->pgid == pgid ? OBOS_STATUS_SUCCESS : OBOS_STATUS_ACCESS_DENIED; // EPERM?

	process_group key = {.pgid=pgid};
	
	Core_MutexAcquire(&Core_ProcessGroupTreeLock);
	
	process_group* pgrp = RB_FIND(process_group_tree, &Core_ProcessGroups, &key);
	if (pgrp)
		goto found;

	pgrp = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*pgrp), nullptr);
	pgrp->leader = proc;
	pgrp->lock = MUTEX_INITIALIZE();
	pgrp->pgid = pgid;
	// TODO(oberrow): When sessions are implemented, take the controlling tty
	// from there.
	if (Core_GetCurrentThread()->proc && Core_GetCurrentThread()->proc->pgrp)
		pgrp->controlling_tty = Core_GetCurrentThread()->proc->pgrp->controlling_tty;
	RB_INSERT(process_group_tree, &Core_ProcessGroups, pgrp);
	
	found:
	Core_MutexRelease(&Core_ProcessGroupTreeLock);

	Core_MutexAcquire(&pgrp->lock);
	LIST_APPEND(process_list, &pgrp->processes, proc);
	proc->pgrp = pgrp;
	Core_MutexRelease(&pgrp->lock);

	return OBOS_STATUS_SUCCESS;
}

obos_status Core_GetProcessGroup(process* proc, uint32_t* pgid)
{
	if (!proc || !pgid)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!proc->pgrp)
		return OBOS_STATUS_INVALID_OPERATION;
	*pgid = proc->pgrp->pgid;
	return OBOS_STATUS_SUCCESS;
}
