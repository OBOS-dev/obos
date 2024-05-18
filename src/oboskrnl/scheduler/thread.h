/*
	oboskrnl/scheduler/thread.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread_context_info.h>

#include <locks/spinlock.h>

typedef enum
{
	THREAD_PRIORITY_INVALID = -1,
	/// <summary>
	/// An idle thread. This thread gets a max quantum of two.
	/// </summary>
	THREAD_PRIORITY_IDLE,
	/// <summary>
	/// A low-priority thread. This thread gets a max quantum of four.
	/// </summary>
	THREAD_PRIORITY_LOW,
	/// <summary>
	/// A normal-priority thread. This thread gets a max quantum of eight.
	/// </summary>
	THREAD_PRIORITY_NORMAL,
	/// <summary>
	/// A high-priority thread. This thread gets a max quantum of 12.
	/// </summary>
	THREAD_PRIORITY_HIGH,
	/// <summary>
	/// A thread requiring urgent priority. This thread gets a max quantum of 12.<br></br>
	/// The difference between this and THREAD_PRIORITY_HIGH is that threads in the list for THREAD_PRIORITY_URGENT are processed before those of THREAD_PRIORITY_HIGH.
	/// </summary>
	THREAD_PRIORITY_URGENT,
	THREAD_PRIORITY_MAX_VALUE = THREAD_PRIORITY_URGENT,
	//PRIORITY_IF_YOU_DONT_RUN_THIS_RIGHT_NOW_THE_KERNEL_DIES,
} thread_priority;
typedef enum
{
	THREAD_FLAGS_APC = 0x01,
	THREAD_FLAGS_DIED = 0x02,
	THREAD_FLAGS_PRIORITY_RAISED = 0x4,
} thread_flags;
typedef enum
{
	THREAD_STATUS_READY = 0,
	THREAD_STATUS_RUNNING,
	THREAD_STATUS_BLOCKED,
} thread_status;
typedef __uint128_t thread_affinity;
extern const uint8_t Core_ThreadPriorityToQuantum[THREAD_PRIORITY_MAX_VALUE + 1];
typedef struct thread
{
	uint64_t tid;
	thread_flags flags;

	size_t references;
	void(*free)(struct thread* what);

	thread_status status;
	thread_priority priority;
	uint8_t quantum;
	thread_affinity affinity;
	uint64_t lastRunTick;
	struct cpu_local* masterCPU /* the cpu that contain this thread's priority list. */;
	struct thread_node* snode;
	
	thread_ctx context;
} thread;
typedef struct thread_node
{
	struct thread_node *next, *prev;
	thread* data;
	void(*free)(struct thread_node* what);
} thread_node;
typedef struct thread_list
{
	thread_node *head, *tail;
	size_t nNodes;
	spinlock lock;
} thread_list;
typedef struct thread_priority_list
{
	thread_list list;
	size_t noStarvationQuantum;
	size_t quantum;
	thread_priority priority;
} thread_priority_list;
thread* CoreH_ThreadAllocate(obos_status* status);
obos_status CoreH_ThreadInitialize(thread* thr, thread_priority priority, thread_affinity affinity, const thread_ctx* ctx);
obos_status CoreH_ThreadReady(thread* thr);
obos_status CoreH_ThreadReadyNode(thread* thr, thread_node* node);
obos_status CoreH_ThreadBlock(thread* thr, thread_node* node, bool canYield);
obos_status CoreH_ThreadListAppend(thread_list* list, thread_node* node);
obos_status CoreH_ThreadListRemove(thread_list* list, thread_node* node);
uint32_t CoreH_CPUIdToAffinity(uint32_t cpuId);
OBOS_NORETURN void CoreH_ExitCurrentThread();