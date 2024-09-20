/*
	oboskrnl/scheduler/cpu_local.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread.h>
#include <irq/dpc.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#ifdef __x86_64__
#	include <arch/x86_64/cpu_local_arch.h>
#elif defined(__m68k__)
#	include <arch/m68k/cpu_local_arch.h>
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
	dpc_queue dpcs;
	spinlock dpc_queue_lock;
	struct {
		// in native timer ticks
		uint64_t work_balancer; 
		uint64_t priority_booster;
		uint64_t total; // between start of Core_Schedule and right before the scheduler does the ctx switch
		uint64_t work_balancer_total; 
		uint64_t priority_booster_total;
		uint64_t total2; // always set to total2 + total
		size_t work_balancer_iterations;
		size_t priority_booster_iterations;
		size_t total2_iterations;
	} sched_profile_data;
} cpu_local;
extern DRV_EXPORT cpu_local* Core_CpuInfo;
extern DRV_EXPORT size_t Core_CpuCount;

#ifdef OBOS_KERNEL
OBOS_WEAK cpu_local* CoreS_GetCPULocalPtr();
#elif defined(OBOS_DRIVER)
OBOS_EXPORT cpu_local* CoreS_GetCPULocalPtr();
#endif