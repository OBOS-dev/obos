/*
 * oboskrnl/arch/x86_64/ssignal.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <signal.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/asm_helpers.h>

#include <locks/event.h>

#include <mm/context.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread_context_info.h>

#define FS_BASE (0xC0000100)
#define GS_BASE (0xC0000101)
#define KERNEL_GS_BASE (0xC0000102)

#define ALLOWED_FLAGS (0b1000011000110111111111)
void OBOSS_SigReturn(interrupt_frame* frame)
{
    // Use frame->rsp to restore the previous thread context.
    ucontext_t ctx = {};
    if (obos_is_error(memcpy_usr_to_k(&ctx, (void*)frame->rsp, sizeof(ctx))))
        return;
    Core_EventClear(&Core_GetCurrentThread()->signal_info->event);
    wrmsr(KERNEL_GS_BASE, ctx.gs_base);
    wrmsr(FS_BASE, ctx.fs_base);
    memcpy(frame, &ctx.frame, sizeof(*frame));
    frame->cs = 0x18|3;
    frame->ss = 0x20|3;
    frame->ds = 0x20|3;
    frame->rflags |= RFLAGS_INTERRUPT_ENABLE;
    frame->rflags &= ~ALLOWED_FLAGS;
    frame->cr3 = CoreS_GetCPULocalPtr()->currentContext->pt;
    // TODO: Restore extended context.
}
void OBOSS_RunSignalImpl(int sigval, interrupt_frame* frame)
{
    if (!(frame->cs & 0x3))
        return;
    sigaction* sig = &Core_GetCurrentThread()->signal_info->signals[sigval];
    ucontext_t ctx = { .frame=*frame,.gs_base=rdmsr(KERNEL_GS_BASE),.fs_base=rdmsr(FS_BASE) };
    if (Core_GetCurrentThread()->context.extended_ctx_ptr)
    {
        ctx.extended_ctx_ptr = Core_GetCurrentThread()->context.extended_ctx_ptr;
        xsave(ctx.extended_ctx_ptr);
    }
    if (sig->flags & SA_ONSTACK && Core_GetCurrentThread()->signal_info->sp)
        frame->rsp = Core_GetCurrentThread()->signal_info->sp;
    ctx.irql = 0;
    frame->rsp -= sizeof(ctx);
    memcpy_k_to_usr((void*)frame->rsp, &ctx, sizeof(ctx));
    frame->rsp -= sizeof(sig->trampoline_base);
    memcpy_k_to_usr((void*)frame->rsp, &sig->trampoline_base, sizeof(sig->trampoline_base));
    uintptr_t ucontext_loc = frame->rsp;
    if (sig->flags & SA_SIGINFO)
    {
        siginfo_t siginfo = {};
        siginfo.sender = sig->sender;
        siginfo.sigcode = sig->sigcode;
        siginfo.status = sig->status;
        siginfo.udata.integer = sig->udata;
        siginfo.signum = sigval;
        frame->rsp -= sizeof(siginfo);
        memcpy_k_to_usr((void*)frame->rsp, &siginfo, sizeof(siginfo));
        frame->rdi = sigval;
        frame->rsi = frame->rsp;
        frame->rdx = ucontext_loc;
        frame->rip = (uintptr_t)sig->un.sa_sigaction;
    }
    else 
    {
        frame->rdi = sigval;
        frame->rip = (uintptr_t)sig->un.handler;
    }
    // When the irq handler returns it will be in the user signal handler.
    // TODO: Test.
}