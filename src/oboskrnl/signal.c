/*
 * oboskrnl/signal.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

// Something close enough to POSIX Signals
// Abandon all hope, ye who enter here.

#include <int.h>
#include <error.h>
#include <klog.h>
#include <signal.h>
#include <signal_def.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>

#include <allocators/base.h>

#include <irq/irq.h>

#include <locks/mutex.h>
#include <locks/event.h>
#include <locks/wait.h>

#include <utils/list.h>

// Abandon all hope, ye who enter here.

signal_header* OBOSH_AllocateSignalHeader()
{
    signal_header* hdr = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(signal_header), nullptr);
    hdr->lock = MUTEX_INITIALIZE();
    hdr->event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    return hdr;
}

obos_status OBOS_Kill(struct thread* as, struct thread* thr, int sigval)
{
    if (!as || !thr || !(sigval >= 0 && sigval <= SIGMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!thr->proc)
        return OBOS_STATUS_INVALID_OPERATION;
    if (thr->proc->pid == 0)
        return OBOS_STATUS_INVALID_OPERATION;
    if (sigval == 0)
        return OBOS_STATUS_SUCCESS; // see man fork.2, "If sig is 0, then no signal is sent, but existence and permission checks are still performed"

    // printf("Sending signal %d as thread %d to thread %d\n", sigval, as->tid, thr->tid);

    OBOS_ENSURE(thr->signal_info);

    if (thr->signal_info->pending & BIT(sigval - 1))
        return OBOS_STATUS_SUCCESS;

    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Core_MutexAcquire(&thr->signal_info->lock)))
        return status;

    if (sigval == SIGCONT)
    {
        if (~thr->signal_info->pending & BIT(SIGSTOP))
        {
            // SIGSTOP is not pending, if the thread is blocked, then ready it and exit.
            if (thr->status == THREAD_STATUS_BLOCKED)
            {
                CoreH_ThreadReady(thr);
                Core_MutexRelease(&thr->signal_info->lock);
                return OBOS_STATUS_SUCCESS;
            }
        }
    }
    else if (sigval == SIGSTOP)
    {
        // Stop the thread.
        thr->proc->signal_handlers[sigval].sender = as;
        CoreH_ThreadBlock(thr, true);
        Core_MutexRelease(&thr->signal_info->lock);
        return OBOS_STATUS_SUCCESS;
    }
    if (thr->proc->signal_handlers[sigval-1].un.handler == SIG_IGN)
    {
        Core_MutexRelease(&thr->signal_info->lock);
        return OBOS_STATUS_SUCCESS;
    }
    thr->signal_info->pending |= BIT(sigval - 1);
    // set these up before the call to kill.
    // thr->proc->signal_handlers[sigval].addr = nullptr;
    // thr->proc->signal_handlers[sigval].status = 0;
    // thr->proc->signal_handlers[sigval].udata = 0;
    // thr->proc->signal_handlers[sigval].sigcode = 0;
    thr->proc->signal_handlers[sigval].sender = as;
    Core_MutexRelease(&thr->signal_info->lock);
    if (thr->status == THREAD_STATUS_BLOCKED)
    {
        if (thr->signal_info->mask & BIT(SIGTSTP - 1))
            return OBOS_STATUS_SUCCESS;
        // TODO: Use CoreH_AbortWaitingThreads instead of this?
        thr->interrupted = true;
        if (!thr->inWaitProcess)
            thr->signalInterrupted = true; 
        CoreH_ThreadReady(thr);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigAction(int signum, const sigaction* act, sigaction* oldact)
{
    if (!(signum >= 0 && signum < SIGMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (oldact)
        *oldact = Core_GetCurrentThread()->proc->signal_handlers[signum];
    if (act)
    {
        Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
        Core_GetCurrentThread()->proc->signal_handlers[signum - 1] = *act;
        // what.
        // Core_GetCurrentThread()->proc->signal_handlers[signum].un.handler = SIG_DFL /* default handler is the kernel */;
        Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigSuspend(sigset_t mask)
{
    Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
    sigset_t old = Core_GetCurrentThread()->signal_info->mask;
    Core_GetCurrentThread()->signal_info->mask = mask;
    obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(Core_GetCurrentThread()->signal_info->event));
    if (obos_is_error(status)) return status;
    Core_GetCurrentThread()->signal_info->mask = old;
    Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigPending(sigset_t* mask)
{
    if (!mask)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *mask = Core_GetCurrentThread()->signal_info->pending;
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigProcMask(int how, const sigset_t* mask, sigset_t* oldset)
{
    if (!mask && oldset)
    {
        *oldset = Core_GetCurrentThread()->signal_info->mask;
        return OBOS_STATUS_SUCCESS;
    }
    if (oldset)
        *oldset = Core_GetCurrentThread()->signal_info->mask;
    if (mask)
    {
        Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
        sigset_t newmask = *mask;
        // These signals cannot be ignored.
        newmask &= ~BIT(SIGKILL-1);
        newmask &= ~BIT(SIGSTOP-1);
        switch (how) {
            case SIG_BLOCK:
                Core_GetCurrentThread()->signal_info->mask |= newmask;
                break;
            case SIG_SETMASK:
                Core_GetCurrentThread()->signal_info->mask = newmask;
                break;
            case SIG_UNBLOCK:
                Core_GetCurrentThread()->signal_info->mask &= ~newmask;
                break;
            default:
                Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
                return OBOS_STATUS_INVALID_ARGUMENT;
        }
        Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigAltStack(const uintptr_t* sp, uintptr_t* oldsp)
{
    if (oldsp)
        *oldsp = Core_GetCurrentThread()->signal_info->sp;
    if (sp)
    {
        Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
        Core_GetCurrentThread()->signal_info->sp = *sp;
        Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status OBOS_KillProcess(struct process* proc, int sigval)
{
    if (!proc || !(sigval >= 0 && sigval <= SIGMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (proc->pid == 0)
        return OBOS_STATUS_INVALID_OPERATION;
    
    proc->refcount++;
    thread* ready = nullptr;
    thread* running = nullptr;
    thread* blocked = nullptr;
    for (thread_node* node = proc->threads.head; node; )
    {
        thread* const thr = node->data;
        node = node->next;

        switch (sigval)
        {
            case SIGCONT:
            case SIGSTOP:
            case SIGTSTP:
                OBOS_Kill(Core_GetCurrentThread(), thr, sigval);
                continue;
            default:
                break;
        }

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

    switch (sigval)
    {
        case SIGCONT:
        {
            // look at linux's WIFCONTINUED for reference
            proc->exitCode = 0xffff;
            CoreH_SignalWaitingThreads(WAITABLE_OBJECT(*proc), true, false);
            return OBOS_STATUS_SUCCESS;
        }
        case SIGSTOP:
        case SIGTSTP:
        {
            // look at linux's WIFSTOPPED for reference
            proc->exitCode = 0x007f;
            CoreH_SignalWaitingThreads(WAITABLE_OBJECT(*proc), true, false);
            return OBOS_STATUS_SUCCESS;
        }
        default:
            break;
    }

    obos_status status = OBOS_STATUS_SUCCESS;

    if (running)
        status = OBOS_Kill(Core_GetCurrentThread(), running, sigval);
    else if (ready)
        status = OBOS_Kill(Core_GetCurrentThread(), ready, sigval);
    else if (blocked)
        status = OBOS_Kill(Core_GetCurrentThread(), blocked, sigval);
    else
        status = OBOS_STATUS_NOT_FOUND;

    return status;
}

obos_status OBOS_KillProcessGroup(process_group* pgrp, int sigval)
{
    if (!pgrp || !(sigval >= 0 && sigval <= SIGMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    for (process* proc = LIST_GET_HEAD(process_list, &pgrp->processes); proc; )
    {
        process* const next = LIST_GET_NEXT(process_list, &pgrp->processes, proc);
        obos_status status = OBOS_KillProcess(proc, sigval);
        if (obos_is_error(status))
            return status;
        proc = next;
    }
    return OBOS_STATUS_SUCCESS;
}

void OBOS_RunSignal(int sigval, interrupt_frame* frame)
{
    sigaction* sig = &Core_GetCurrentThread()->proc->signal_handlers[sigval];
    if (!(sig->flags & SA_NODEFER))
        Core_GetCurrentThread()->signal_info->mask |= BIT_TYPE(sigval-1, L);
    Core_EventSet(&Core_GetCurrentThread()->signal_info->event, false);
    OBOSS_RunSignalImpl(sigval, frame);
    if (sig->flags & SA_RESETHAND && sigval != SIGILL && sigval != SIGTRAP)
    {
        sig->flags &= ~SA_SIGINFO;
        sig->un.handler = SIG_DFL;
    }
}
OBOS_NO_UBSAN bool OBOS_SyncPendingSignal(interrupt_frame* frame)
{
    if (!CoreS_GetCPULocalPtr()->currentThread)
        return false;
    if (!Core_GetCurrentThread()->signal_info)
        return false;
    if (!(Core_GetCurrentThread()->signal_info->pending & ~Core_GetCurrentThread()->signal_info->mask))
        return false;
    Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
    int sigval = __builtin_ctzll(Core_GetCurrentThread()->signal_info->pending & ~Core_GetCurrentThread()->signal_info->mask);
    Core_GetCurrentThread()->signal_info->pending &= ~BIT_TYPE(sigval, L);
    sigval += 1;
    Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    OBOS_RunSignal(sigval, frame);
    return Core_GetCurrentThread()->proc->signal_handlers[sigval-1].un.handler != SIG_IGN;
}

#define T SIGNAL_DEFAULT_TERMINATE_PROC,
#define A SIGNAL_DEFAULT_TERMINATE_PROC,
#define I SIGNAL_DEFAULT_IGNORE,
#define C SIGNAL_DEFAULT_CONTINUE,
#define S SIGNAL_DEFAULT_STOP,
enum signal_default_action OBOS_SignalDefaultActions[SIGMAX+1] = {
    T
    T T T A A
    A A A T T
    A T T T T
    A I C S S
    S S I A A
    T A
};
void OBOS_DefaultSignalHandler(int signum, siginfo_t* info, void* unknown)
{
    OBOS_UNUSED(signum);
    OBOS_UNUSED(info);
    OBOS_UNUSED(unknown);
    switch (OBOS_SignalDefaultActions[signum]) {
        case SIGNAL_DEFAULT_TERMINATE_PROC:
            OBOS_Debug("Exitting process %ld after receiving signal %d\n", Core_GetCurrentThread()->proc->pid, signum);
            break;
        case SIGNAL_DEFAULT_IGNORE:
        case SIGNAL_DEFAULT_STOP:
        case SIGNAL_DEFAULT_CONTINUE:
            OBOS_ASSERT(!"signal handled in the wrong place\n");
            break;
        default:
            OBOS_ASSERT(!"unknown signal default action");
    }
    Free(OBOS_NonPagedPoolAllocator, info, sizeof(*info));
    Free(OBOS_NonPagedPoolAllocator, unknown, sizeof(ucontext_t));
    Core_ExitCurrentProcess(signum | (signum << 8));
}
