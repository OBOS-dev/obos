/*
 * oboskrnl/signal.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

// Something close enough to POSIX Signals

#include <int.h>
#include <error.h>
#include <signal.h>

#include <scheduler/schedule.h>

#include <irq/irq.h>

#include <locks/mutex.h>
#include <locks/event.h>
#include <locks/wait.h>

#include <utils/list.h>

obos_status OBOS_Kill(struct thread* as, struct thread* thr, int sigval)
{
    if (!as || !thr || !(sigval >= 0 && sigval <= SIGRTMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (thr->signal_info->pending & BIT(sigval))
        return OBOS_STATUS_SUCCESS;
    Core_MutexAcquire(&thr->signal_info->lock);
    thr->signal_info->pending |= BIT(sigval);
    // set these up before the call to kill.
    // thr->signal_info->signals[sigval].addr = nullptr;
    // thr->signal_info->signals[sigval].status = 0;
    // thr->signal_info->signals[sigval].udata = 0;
    // thr->signal_info->signals[sigval].sigcode = 0;
    thr->signal_info->signals[sigval].sender = as;
    Core_MutexRelease(&thr->signal_info->lock);
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigAction(int signum, const sigaction* act, sigaction* oldact)
{
    if (!(signum >= 0 && signum <= SIGRTMAX))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (oldact)
        *oldact = Core_GetCurrentThread()->signal_info->signals[signum];
    if (act)
    {
        Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
        Core_GetCurrentThread()->signal_info->signals[signum] = *act;
        Core_GetCurrentThread()->signal_info->signals[signum].un.handler = SIG_DFL /* default handler is the kernel */;
        Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status OBOS_SigSuspend(sigset_t mask)
{
    Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
    sigset_t old = Core_GetCurrentThread()->signal_info->mask;
    Core_GetCurrentThread()->signal_info->mask = mask;
    Core_WaitOnObject(WAITABLE_OBJECT(Core_GetCurrentThread()->signal_info->event));
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

void OBOS_RunSignal(int sigval, interrupt_frame* frame)
{
    sigaction* sig = &Core_GetCurrentThread()->signal_info->signals[sigval];
    if (!(sig->flags & SA_NODEFER))
        Core_GetCurrentThread()->signal_info->mask |= BIT(sigval);
    Core_EventSet(&Core_GetCurrentThread()->signal_info->event, false);
    OBOSS_RunSignalImpl(sigval, frame);
    if (sig->flags & SA_RESETHAND && sigval != SIGILL && sigval != SIGTRAP)
    {
        sig->flags &= ~SA_SIGINFO;
        sig->un.handler = SIG_DFL;
    }
}
void OBOS_SyncPendingSignal(interrupt_frame* frame)
{
    if (!Core_GetCurrentThread()->signal_info->pending)
        return;
    Core_MutexAcquire(&Core_GetCurrentThread()->signal_info->lock);
    int sigval = __builtin_ctzll(Core_GetCurrentThread()->signal_info->pending & ~Core_GetCurrentThread()->signal_info->mask);
    Core_GetCurrentThread()->signal_info->pending &= ~BIT(sigval);
    OBOS_RunSignal(sigval, frame);
    Core_MutexRelease(&Core_GetCurrentThread()->signal_info->lock);
}