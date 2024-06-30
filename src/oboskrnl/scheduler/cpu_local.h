/*
	oboskrnl/scheduler/cpu_local.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#ifdef __x86_64__
#	include <arch/x86_64/cpu_local_arch.h>
#endif

typedef struct cpu_local
{
	uint32_t id;
	bool isBSP;
	struct thread* currentThread;
	struct thread* idleThread;
	struct context* currentContext;
	cpu_local_arch arch_specific;
	// Only threads that are ready can go in one of these thread lists.
	thread_priority_list priorityLists[THREAD_PRIORITY_MAX_VALUE + 1];
	thread_priority_list* currentPriorityList;
	spinlock schedulerLock;
	uint64_t schedulerTicks;
	irql currentIrql;
	bool initialized;
} cpu_local;
extern cpu_local* Core_CpuInfo;
extern size_t Core_CpuCount;

// Must be initialized by the arch.
extern void* Core_CpuTempStackBase; // The base of the temp stacks (they must be allocated in a contiguous block)
extern size_t Core_CpuTempStackSize; // The size of one cpu temp stack.
cpu_local* CoreS_GetCPULocalPtr();