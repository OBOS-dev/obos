/*
	oboskrnl/scheduler/thread.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread_context_info.h>

#include <locks/spinlock.h>

#if OBOS_ENABLE_PROFILING && defined(OBOS_KERNEL)
#	include <prof.h>
#endif

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
	/// A thread requiring urgent priority. This thread gets a max quantum of 12.<para/>
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
	THREAD_FLAGS_DEBUGGER_BLOCKED = 0x8, // kernel mode flag only
} thread_flags;
typedef enum
{
	THREAD_STATUS_INVALID = 0,
	THREAD_STATUS_READY = 1,
	THREAD_STATUS_RUNNING,
	THREAD_STATUS_BLOCKED,
} thread_status;
#if defined(__x86_64__)
typedef __uint128_t thread_affinity;
#elif defined(__m68k__)
typedef uint64_t thread_affinity;
#endif
extern OBOS_EXPORT thread_affinity Core_DefaultThreadAffinity;
extern const uint8_t Core_ThreadPriorityToQuantum[THREAD_PRIORITY_MAX_VALUE+1];
typedef struct thread_node
{
	struct thread_node *next, *prev;
	struct thread* data;
	void(*free)(struct thread_node* what);
} thread_node;
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
	struct thread_node* pnode;
	struct process* proc;
	
	thread_ctx context;
	// Frees the thread's stack.
	void* stackFreeUserdata;
	void(*stackFree)(void* base, size_t sz, void* userdata);

	// The node used by waitable_header (locks/wait.h)
	thread_node lock_node;
	size_t nWaiting; // the count of objects the thread is waiting on.
	size_t nSignaled; // the count of objects that have signaled the thread.
	struct waitable_header* hdrSignaled;
	bool interrupted : 1;
	bool signalInterrupted : 1; // if interrupted is true because of a signal.
	bool inWaitProcess : 1;
	
	struct signal_header* signal_info;

	void* kernelStack; // size: 0x10000 bytes
	void* userStack; // size: 0x10000 bytes, used for signals dispatched in kernel-mode.

	// Thread profiling info

#if OBOS_ENABLE_PROFILING && defined(OBOS_KERNEL)
	call_frame_t frames[MAX_FRAMES];
	size_t cur_frame;
#else
	uintptr_t resv1[2];
	uint64_t resv2[2];
#endif

	// The amount of quantums the thread has ever ran for.
	uint8_t total_quantums;
} thread;
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
/// <summary>
/// Allocates a thread.
/// </summary>
/// <param name="status">[out,opt] The status of the function.</param>
/// <returns>The newly allocated thread.</returns>
OBOS_EXPORT thread* CoreH_ThreadAllocate(obos_status* status);
/// <summary>
/// Initializes a thread.<para/>
/// The thread will not be processed by the scheduler until it is readied.
/// </summary>
/// <param name="thr">The thread.</param>
/// <param name="priority">The priority of the thread.</param>
/// <param name="affinity">The affinity of the thread.</param>
/// <param name="ctx">The thread context.</param>
/// <returns>The function status.</returns>
OBOS_EXPORT obos_status CoreH_ThreadInitialize(thread* thr, thread_priority priority, thread_affinity affinity, const thread_ctx* ctx);
/// <summary>
/// Readies a thread.
/// </summary>
/// <param name="thr">The thread to ready.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status CoreH_ThreadReady(thread* thr);
/// <summary>
/// Readies a thread, but uses a preallocated node.
/// </summary>
/// <param name="thr">The thread to ready.</param>
/// <param name="node">The node to use.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status CoreH_ThreadReadyNode(thread* thr, thread_node* node);
/// <summary>
/// Blocks a thread.<para/>
/// Yields if the thread is the current thread, unless otherwise specified.
/// </summary>
/// <param name="thr">The thread to block.</param>
/// <param name="canYield">Whether the function is allowed to yield into the scheduler manually.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status CoreH_ThreadBlock(thread* thr, bool canYield);
/// <summary>
/// Boosts a thread's priority.
/// </summary>
/// <param name="thr">The thread.</param>
/// <returns>The function's status.</returns>
OBOS_EXPORT obos_status CoreH_ThreadBoostPriority(thread* thr);
/// <summary>
/// Appends a thread to a thread list.
/// </summary>
/// <param name="list">The thread list.</param>
/// <param name="node">A node containing the thread.</param>
/// <returns>The function's status.</returns>
OBOS_EXPORT obos_status CoreH_ThreadListAppend(thread_list* list, thread_node* node);
/// <summary>
/// Removes a thread from a thread list.
/// </summary>
/// <param name="list">The thread list.</param>
/// <param name="node">The node containing the thread.</param>
/// <returns>The function's status/</returns>
OBOS_EXPORT obos_status CoreH_ThreadListRemove(thread_list* list, thread_node* node);
/// <summary>
/// Converts a cpu id to an affinity mask.
/// </summary>
/// <param name="cpuId">The cpu id.</param>
/// <returns>The affinity mask.</returns>
OBOS_EXPORT thread_affinity CoreH_CPUIdToAffinity(uint32_t cpuId);
/// <summary>
/// Exits the current thread.
/// </summary>
OBOS_NORETURN OBOS_EXPORT void Core_ExitCurrentThread();
