/*
	oboskrnl/scheduler/thread.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <scheduler/thread_context_info.h>

typedef enum
{
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
	THREAD_PRIOIRTY_MAX_VALUE = THREAD_PRIORITY_URGENT,
	//PRIORITY_IF_YOU_DONT_RUN_THIS_RIGHT_NOW_THE_KERNEL_DIES,
} thread_priority;
typedef enum
{
	THREAD_FLAGS_APC = 0x01,
	THREAD_FLAGS_DIED = 0x02,
} thread_flags;
typedef enum
{
	THREAD_STATUS_CAN_RUN = 0,
	THREAD_STATUS_RUNNING,
	THREAD_STATUS_BLOCKED,
} thread_status;
typedef __uint128_t thread_affinity;
extern const uint8_t Core_ThreadPriorityToQuantum[THREAD_PRIOIRTY_MAX_VALUE + 1];
typedef struct __thread
{
	uint32_t tid;
	thread_flags flags;

	thread_priority priority;
	uint8_t quantum;
	thread_affinity affinity;
	
	thread_ctx context;
} thread;
typedef struct __thread_node
{
	struct __thread_node *next, *prev;
	thread* data;
} thread_node;
typedef struct __thread_list
{
	thread_node *head, *tail;
	size_t nNodes;
} thread_list;