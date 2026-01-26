/*
 * oboskrnl/scheduler/process.h
 * 
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <handle.h>
#include <signal_def.h>

#include <vfs/dirent.h>

#include <vfs/tty.h>

#include <locks/spinlock.h>
#include <locks/wait.h>
#include <locks/mutex.h>

#include <scheduler/thread.h>

#include <utils/list.h>

typedef struct process
{
	// Processes waiting for a status update.
	struct waitable_header waiting_threads;

	// If pid==0, this is the kernel process.
	uint32_t pid;
	thread_list threads;
	struct context* ctx;
	handle_table handles;
	_Atomic(size_t) refcount;

	struct process_group* pgrp;
	struct session* session;

	uid euid, ruid, suid;
	gid egid, rgid, sgid;
	struct {
		gid *list;
		size_t nEntries;
	} groups;
	
	uint32_t exitCode;
	bool dead;

	struct process* parent;
	struct {
		struct process *head, *tail;
		size_t nChildren;
	} children;
	spinlock children_lock;
	struct process *next, *prev;

	dirent* cwd;
	const char* cwd_str;

	char* exec_file;
	char* cmdline;

	// Only exists for POSIX, ignored in most parts of the kernel.
	uint32_t umask;

    sigaction signal_handlers[64];

	LIST_NODE(process_list, struct process) node;
} process;
typedef LIST_HEAD(process_list, process) process_list;
LIST_PROTOTYPE(process_list, process, node);

typedef struct process_group {
	uint32_t pgid;
	process* leader;
	process_list processes;
	mutex lock;
	RB_ENTRY(process_group) rb_node;
} process_group;
typedef RB_HEAD(process_group_tree, process_group) process_group_tree;
RB_PROTOTYPE(process_group_tree, process_group, node, pgrp_cmp);
extern process_group_tree Core_ProcessGroups;
extern mutex Core_ProcessGroupTreeLock;

typedef struct session {
	uint32_t sid;
	process* leader;
	tty* controlling_tty;
	size_t refs;
} session;

extern uint32_t Core_NextPID;

// The first thread in this process must be the kernel main thread, until the thread exits.
extern OBOS_EXPORT process* OBOS_KernelProcess;
/// <summary>
/// Allocates a process object.
/// </summary>
/// <param name="status">[out,opt] The status of the function.</param>
/// <returns>The object.</returns>
OBOS_EXPORT process* Core_ProcessAllocate(obos_status* status);
/// <summary>
/// Starts a process. Readies the mainThread passed.
/// </summary>
/// <param name="proc">A pointer to the process object.</param>
/// <param name="mainThread">The main thread of the process. Must be initialized, but not readied.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_ProcessStart(process* proc, thread* mainThread);
/// <summary>
/// Appends a thread to a process. Does not ready the thread passed to it.
/// </summary>
/// <param name="proc">The process.</param>
/// <param name="thread">The thread.</param>
/// <returns>The status of the function.</returns>
OBOS_EXPORT obos_status Core_ProcessAppendThread(process* proc, thread* thread);
/// <summary>
/// Terminates the current process.
/// </summary>
/// <param name="code">The exit code of the process.</param>
OBOS_NORETURN void Core_ExitCurrentProcess(uint32_t code);

process* Core_LookupProc(uint64_t pid);

/// <summary>
/// Exits the current process group.
/// </summary>
void Core_ExitProcessGroup();
obos_status Core_SetProcessGroup(process* proc, uint32_t pgid);
obos_status Core_GetProcessGroup(process* proc, uint32_t* pgid);

obos_status Core_MakeSession(process* proc, session** out);