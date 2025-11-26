/*
 * oboskrnl/signal.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

// Something close enough to POSIX Signals

#include <int.h>
#include <error.h>
#include <handle.h>

#include <irq/irq.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>

#include <locks/event.h>
#include <locks/mutex.h>

#include <utils/list.h>

#define SIG_DFL (nullptr)
#define SIG_IGN ((void*)1)

#include <signal_def.h>

signal_header* OBOSH_AllocateSignalHeader();

obos_status OBOS_Kill(struct thread* as, struct thread* thr, int sigval);
obos_status OBOS_SigAction(int signum, const sigaction* act, sigaction* oldact);
obos_status OBOS_SigSuspend(sigset_t mask);
obos_status OBOS_SigPending(sigset_t* mask);
enum {
    SIG_BLOCK,
    SIG_SETMASK,
    SIG_UNBLOCK,
};
obos_status OBOS_SigProcMask(int how, const sigset_t* mask, sigset_t* oldset);
obos_status OBOS_SigAltStack(const uintptr_t* sp, uintptr_t* oldsp);

obos_status OBOS_KillProcess(process* proc, int sigval);
obos_status OBOS_KillProcessGroup(process_group* pgrp, int sigval);

typedef struct thread_context_info ucontext_t;

bool OBOS_SyncPendingSignal(interrupt_frame* frame);
void OBOS_RunSignal(int sigval, interrupt_frame* frame);

// NOTE: This function is implemented as if it is a syscall,
// i.e., frame is `memcpy_usr_to_k`ed to a kernel buffer.
OBOS_WEAK void OBOSS_SigReturn(ucontext_t* frame);

OBOS_WEAK void OBOSS_RunSignalImpl(int sigval, interrupt_frame* frame);

extern enum signal_default_action OBOS_SignalDefaultActions[SIGMAX+1];

void OBOS_DefaultSignalHandler(int signum, siginfo_t* info, void* unknown);

enum {
    SS_DISABLE = BIT(0),
};
typedef struct stack {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

// TODO: Better values?

#define MINSIGSTKSZ 0x20000
#define SIGSTKSZ 0x20000

// Syscalls
obos_status Sys_Kill(handle thr, int sigval);
obos_status Sys_KillProcess(handle proc, int sigval);
obos_status Sys_KillProcessGroup(uint32_t pgid, int sigval);
obos_status Sys_SigAction(int signum, const user_sigaction* act, user_sigaction* oldact);
obos_status Sys_SigPending(sigset_t* mask);
obos_status Sys_SigProcMask(int how, const sigset_t* mask, sigset_t* oldset);
obos_status Sys_SigAltStack(const stack_t* sp, stack_t* oldsp);
