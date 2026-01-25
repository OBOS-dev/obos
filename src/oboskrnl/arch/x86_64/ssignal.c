/*
 * oboskrnl/arch/x86_64/ssignal.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <signal.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/sse.h>

#include <locks/event.h>

#include <mm/context.h>

#include <allocators/base.h>

#include <irq/irql.h>

#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread_context_info.h>

#include <mm/alloc.h>

// Abandon all hope, ye who enter here.

#define FS_BASE (0xC0000100)
#define GS_BASE (0xC0000101)
#define KERNEL_GS_BASE (0xC0000102)

#define ALLOWED_FLAGS (0b1000011000110111111111)
void OBOSS_SigReturn(ucontext_t* uctx)
{
    if (Core_GetCurrentThread()->signal_info->is_siginfo)
        uctx = (void*)((uintptr_t)uctx + sizeof(siginfo_t));
    ucontext_t* ctx = Mm_MapViewOfUserMemory(Core_GetCurrentThread()->proc->ctx, uctx, nullptr, sizeof(ucontext_t), OBOS_PROTECTION_READ_ONLY, true, nullptr);
    if (!ctx)
        return;
    Core_EventClear(&Core_GetCurrentThread()->signal_info->event);
    irql oldIrql = Core_RaiseIrqlNoThread(IRQL_DISPATCH);
    OBOS_UNUSED(oldIrql);
    thread_ctx* thread_ctx = &Core_GetCurrentThread()->context;
    memcpy(&thread_ctx->frame, &ctx->frame, sizeof(interrupt_frame));
    thread_ctx->cr3 = ctx->cr3;
    thread_ctx->fs_base = ctx->fs_base;
    thread_ctx->gs_base = ctx->gs_base;
    if (thread_ctx->signal_extended_ctx_ptr && thread_ctx->extended_ctx_ptr)
        memcpy(thread_ctx->extended_ctx_ptr, thread_ctx->signal_extended_ctx_ptr, Arch_GetXSaveRegionSize());
    thread_ctx->irql = ctx->irql;
    Mm_VirtualMemoryFree(&Mm_KernelContext, ctx, sizeof(*ctx));
    CoreS_SwitchToThreadContext(thread_ctx);
}
void OBOSS_RunSignalImpl(int sigval, interrupt_frame* frame)
{
    // if (!(frame->cs & 0x3))
    //     return;
    sigaction* sig = &Core_GetCurrentThread()->proc->signal_handlers[sigval-1];
    if (sig->un.handler == SIG_IGN)
        return;
    ucontext_t ctx = { .frame=*frame,.gs_base=rdmsr(!(frame->cs & 0x3) ? GS_BASE : KERNEL_GS_BASE),.fs_base=rdmsr(FS_BASE),.cr3=frame->cr3 };
    if (!Core_GetCurrentThread()->context.signal_extended_ctx_ptr)
        Core_GetCurrentThread()->context.signal_extended_ctx_ptr = Arch_AllocateXSAVERegion();
    if (Arch_HasXSAVE)
        xsave(Core_GetCurrentThread()->context.signal_extended_ctx_ptr);
    else
        __builtin_ia32_fxsave(Core_GetCurrentThread()->context.signal_extended_ctx_ptr);
    bool is_kernel_stack = !(frame->cs & 0x3);
    if (sig->flags & SA_ONSTACK && Core_GetCurrentThread()->signal_info->sp)
        frame->rsp = Core_GetCurrentThread()->signal_info->sp;
    if (is_kernel_stack)
    {
        if (!Core_GetCurrentThread()->userStack)
            Core_GetCurrentThread()->userStack = Mm_VirtualMemoryAlloc(Core_GetCurrentThread()->proc->ctx, nullptr, 0x10000, 0, VMA_FLAGS_GUARD_PAGE, nullptr, nullptr);
        OBOS_ENSURE(Core_GetCurrentThread()->userStack);
        frame->rsp = (uintptr_t)Core_GetCurrentThread()->userStack + 0x10000;
    }
    
    ctx.irql = 0;
    
    frame->rsp -= sizeof(ctx);
    uint8_t *rsp = Mm_MapViewOfUserMemory(Core_GetCurrentThread()->proc->ctx, (void*)frame->rsp, nullptr, sizeof(ucontext_t), 0, true, nullptr);
    if (!rsp)
        Core_ExitCurrentThread();
    memcpy(rsp, &ctx, sizeof(ctx));
    Mm_VirtualMemoryFree(&Mm_KernelContext, rsp, sizeof(ucontext_t));
    
    uintptr_t ucontext_loc = frame->rsp;
    if (!sig->un.handler || sigval == SIGKILL || sigval == SIGSTOP)
    {
        switch (OBOS_SignalDefaultActions[sigval]) {
            case SIGNAL_DEFAULT_TERMINATE_PROC:
                break;
            case SIGNAL_DEFAULT_IGNORE:
                return;
            case SIGNAL_DEFAULT_STOP:
                CoreH_ThreadBlock(Core_GetCurrentThread(), true);
                return;
            case SIGNAL_DEFAULT_CONTINUE:
                return;
            default:
                OBOS_ASSERT(!"unknown signal default action");
        }
        // bruh
        siginfo_t siginfo = {};
        siginfo.sender = sig->sender;
        siginfo.sigcode = sig->sigcode;
        siginfo.status = sig->status;
        siginfo.udata.integer = sig->udata;
        siginfo.signum = sigval;
        frame->rdi = sigval;
        frame->rsi = (uintptr_t)ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(siginfo), nullptr);
        memcpy((void*)frame->rsi, &siginfo, sizeof(siginfo));
        frame->rdx = (uintptr_t)ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(ctx), nullptr);
        memcpy((void*)frame->rdx, &ctx, sizeof(ctx));
        frame->rip = (uintptr_t)OBOS_DefaultSignalHandler;
        frame->cs = 0x8;
        frame->ss = 0x10;
        frame->ds = 0x10;
        frame->cr3 = MmS_GetCurrentPageTable();
        frame->rsp = (uintptr_t)Core_GetCurrentThread()->kernelStack + 0x10000;
        return;
    }
    frame->cr3 = Core_GetCurrentThread()->proc->ctx->pt;
    frame->rflags = 0x200202;
    frame->cs = 0x23;
    frame->ss = 0x1b;
    frame->ds = 0x1b;
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
        frame->rsp -= sizeof(sig->trampoline_base);
        Core_GetCurrentThread()->signal_info->is_siginfo = true;
        memcpy_k_to_usr((void*)frame->rsp, &sig->trampoline_base, sizeof(sig->trampoline_base));
        frame->rdi = sigval;
        frame->rsi = frame->rsp;
        frame->rdx = ucontext_loc;
        frame->rip = (uintptr_t)sig->un.sa_sigaction;
    }
    else
    {
        frame->rdi = sigval;
        frame->rip = (uintptr_t)sig->un.handler;
        frame->rsp -= sizeof(sig->trampoline_base);
        memcpy_k_to_usr((void*)frame->rsp, &sig->trampoline_base, sizeof(sig->trampoline_base));
    }
    // When the irq handler returns it will be in the user signal handler.
    // NOTTODO: Test.
    // NOTE(oberrow): HOW DID THIS WORK FIRST TRY!!!!
    // LESGOOOOOOOOOOOOOOOOOOoooo
}
