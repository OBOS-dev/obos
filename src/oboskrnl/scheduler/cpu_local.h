/*
	oboskrnl/scheduler/cpu_local.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>

#include <irq/irql.h>

#ifdef __x86_64__
#	include <arch/x86_64/cpu_local_arch.h>
#endif

typedef struct cpu_local
{
	uint32_t id;
	bool isBSP;
	struct thread* currentThread;
	struct thread* idleThread;
	cpu_local_arch arch_specific;
	// Only threads that are ready can go in one of these thread lists.
	thread_priority_list priorityLists[THREAD_PRIORITY_MAX_VALUE + 1];
	thread_priority_list* currentPriorityList;
	uint64_t schedulerTicks;
	irql currentIrql;
	bool initialized;
} cpu_local;
extern cpu_local* Core_CpuInfo;
extern size_t Core_CpuCount;
cpu_local* CoreS_GetCPULocalPtr();