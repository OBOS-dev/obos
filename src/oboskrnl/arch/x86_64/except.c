/*
 * oboskrnl/arch/x86_64/except.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <signal.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/idt.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <mm/init.h>
#include <mm/handler.h>
#include <mm/context.h>

#include "gdbstub/debug.h"

uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr);
__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void Arch_PageFaultHandler(interrupt_frame* frame)
{
    sti();
    uintptr_t virt = getCR2();
    // for (volatile bool b = (virt == 0x412134fa); b; )
    //     asm volatile ("" : "=r"(b) : "r"(b) :"memory");
    virt &= ~0xfff;
    if (!CoreS_GetCPULocalPtr())
        goto down;
    if (!CoreS_GetCPULocalPtr()->currentContext)
        goto down;
    if (Arch_GetPML2Entry(CoreS_GetCPULocalPtr()->currentContext->pt, virt) & (1<<7))
        virt &= ~0x1fffff;
    if (Core_GetIrql() > IRQL_DISPATCH)
        OBOS_Error("Page fault at > IRQL_DISPATCH\n");
    if (Mm_IsInitialized() && Core_GetIrql() <= IRQL_DISPATCH)
    {
        CoreS_GetCPULocalPtr()->arch_specific.pf_handler_running = true;
        uint32_t mm_ec = 0;
        if (frame->errorCode & BIT(0))
            mm_ec |= PF_EC_PRESENT;
        if (frame->errorCode & BIT(1))
            mm_ec |= PF_EC_RW;
        if (frame->errorCode & BIT(2))
            mm_ec |= PF_EC_UM;
        if (frame->errorCode & BIT(3))
            mm_ec |= PF_EC_INV_PTE;
        if (frame->errorCode & BIT(4))
            mm_ec |= PF_EC_EXEC;
        // TODO: Find out why CoreS_GetCPULocalPtr()->currentContext is nullptr in the first place
        if (!CoreS_GetCPULocalPtr()->currentContext)
        {
            if (CoreS_GetCPULocalPtr()->currentThread->proc->pid != 0 && mm_ec & PF_EC_UM)
                CoreS_GetCPULocalPtr()->currentContext = CoreS_GetCPULocalPtr()->currentThread->proc->ctx;
            else
                CoreS_GetCPULocalPtr()->currentContext = &Mm_KernelContext;
        }
        irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
        obos_status status = Mm_HandlePageFault(CoreS_GetCPULocalPtr()->currentContext, virt, mm_ec);
        Core_LowerIrql(oldIrql);
        switch (status)
        {
            case OBOS_STATUS_SUCCESS:
                CoreS_GetCPULocalPtr()->arch_specific.pf_handler_running = false;
                OBOS_ASSERT(frame->rsp != 0);
                cli();
                return;
            case OBOS_STATUS_UNHANDLED:
                break;
            default:
            {
                OBOS_Warning("Handling page fault with error code 0x%x on address %p failed with status %d.\n", mm_ec, getCR2(), status);
                break;
            }
        }
    }
    if (Kdbg_CurrentConnection && !Kdbg_Paused && Kdbg_CurrentConnection->connection_active)
    {
        asm("sti");
        irql oldIrql = Core_GetIrql();
        Core_LowerIrqlNoThread(IRQL_PASSIVE);
        Kdbg_NotifyGDB(Kdbg_CurrentConnection, 11 /* SIGSEGV */);
        Kdbg_CallDebugExceptionHandler(frame, true);
        irql discardedlol = Core_RaiseIrqlNoThread(oldIrql);
        (void)discardedlol;
        asm("cli");
    }
    if (frame->cs & 3)
    {
        OBOS_Log("User thread %d SIGSEGV (rip 0x%p, cr2 0x%p, error code 0x%08x)\n", Core_GetCurrentThread()->tid, frame->rip, getCR2(), frame->errorCode);
        Core_GetCurrentThread()->signal_info->signals[SIGSEGV].addr = (void*)getCR2();
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGSEGV);
        // OBOS_SyncPendingSignal(frame);
        OBOS_RunSignal(SIGSEGV, frame); // Ensure SIGSEGV runs.
        return;
    }
    down:
    asm("cli");
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        "Page fault at 0x%p in %s-mode while trying to %s page at 0x%p, which is %s. Error code: %d\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
        (void*)frame->rip,
        frame->cs == 0x8 ? "kernel" : "user",
        (frame->errorCode & 2) ? "write" : ((frame->errorCode & 0x10) ? "execute" : "read"),
        getCR2(),
        frame->errorCode & 1 ? "present" : "unpresent",
        frame->errorCode,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN OBOS_NO_KASAN void Arch_DoubleFaultHandler(interrupt_frame* frame)
{
    static const char format[] = "Double fault!\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n";
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        format,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN OBOS_NO_KASAN void Arch_SegvHandler(interrupt_frame* frame)
{
    if (frame->cs & 3)
    {
        OBOS_Log("User thread %d SIGSEGV\n", Core_GetCurrentThread()->tid);
        if (Kdbg_CurrentConnection && !Kdbg_Paused && Kdbg_CurrentConnection->connection_active)
        {
            asm("sti");
            irql oldIrql = Core_GetIrql();
            Core_LowerIrqlNoThread(IRQL_PASSIVE);
            Kdbg_NotifyGDB(Kdbg_CurrentConnection, 11 /* SIGSEGV */);
            Kdbg_CallDebugExceptionHandler(frame, true);
            irql discardedlol = Core_RaiseIrqlNoThread(oldIrql);
            (void)discardedlol;
            asm("cli");
        }
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGSEGV);
        OBOS_SyncPendingSignal(frame);
        OBOS_RunSignal(SIGSEGV, frame); // Ensure SIGSEGV runs.
        return;
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        "Kernel segmentation fault! Exception code: %d. Error code: 0x%08x\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
        frame->intNumber, frame->errorCode,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN OBOS_NO_KASAN void Arch_UndefinedOpcodeHandler(interrupt_frame* frame)
{
    if (frame->cs & 3)
    {
        OBOS_Log("User thread %d SIGILL\n", Core_GetCurrentThread()->tid);
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGILL);
        // OBOS_SyncPendingSignal(frame);
        OBOS_RunSignal(SIGILL, frame); // Ensure SIGILL runs.
        return;
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        "Kernel illegal instruction! Exception code: %d\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
        frame->intNumber,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN void Arch_FPEHandler(interrupt_frame* frame)
{
    if (frame->cs & 3)
    {
        OBOS_Log("User thread %d SIGFPE (rip 0x%p)\n", Core_GetCurrentThread()->tid, frame->rip);
        if (Kdbg_CurrentConnection && !Kdbg_Paused && Kdbg_CurrentConnection->connection_active)
        {
            asm("sti");
            irql oldIrql = Core_GetIrql();
            Core_LowerIrqlNoThread(IRQL_PASSIVE);
            Kdbg_NotifyGDB(Kdbg_CurrentConnection, SIGFPE);
            Kdbg_CallDebugExceptionHandler(frame, true);
            irql discardedlol = Core_RaiseIrqlNoThread(oldIrql);
            (void)discardedlol;
            asm("cli");
        }
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGFPE);
        // OBOS_SyncPendingSignal(frame);
        OBOS_RunSignal(SIGFPE, frame); // Ensure SIGFPE runs.
        return;
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        "Kernel floating point error! Exception code: %d\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
        frame->intNumber,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}

__attribute__((no_instrument_function)) OBOS_NO_UBSAN OBOS_NO_KASAN OBOS_NO_KASAN void Arch_DbgExceptHandler(interrupt_frame* frame)
{
    // if (Kdbg_CurrentConnection->pipe_interface && Kdbg_CurrentConnection->pipe_interface->read_sync)
    // {
    //     switch (frame->intNumber) {
    //         case 1:
    //             Kdbg_int1_handler(frame);
    //             break;
    //         case 3:
    //             Kdbg_int3_handler(frame);
    //             break;
    //         default:
    //             OBOS_UNREACHABLE; 
    //     }
    //     return;
    // }
    if (frame->cs & 3)
    {
        OBOS_Log("User thread %d SIGTRAP\n", Core_GetCurrentThread()->tid);
        // Doesn't really apply.
        // if (Kdbg_CurrentConnection && !Kdbg_Paused && Kdbg_CurrentConnection->connection_active)
        // {
        //     asm("sti");
        //     irql oldIrql = Core_GetIrql();
        //     Core_LowerIrqlNoThread(IRQL_PASSIVE);
        //     Kdbg_NotifyGDB(Kdbg_CurrentConnection, SIGTRAP);
        //     Kdbg_CallDebugExceptionHandler(frame, true);
        //     (void)Core_RaiseIrqlNoThread(oldIrql);
        //     asm("cli");
        // }
        OBOS_Kill(Core_GetCurrentThread(), Core_GetCurrentThread(), SIGTRAP);
        // OBOS_SyncPendingSignal(frame);
        OBOS_RunSignal(SIGTRAP, frame); // Ensure SIGTRAP runs.
        return;
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION, 
        "Unexpected kernel-mode debug exception! Exception code: %d\n"
        "Register dump:\n"
        "\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
        "\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
        "\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
        "\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
        "\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
        "\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
        "\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
        "\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
        "\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
        frame->intNumber,
        frame->rdi, frame->rsi, frame->rbp,
        frame->rsp, frame->rbx, frame->rdx,
        frame->rcx, frame->rax, frame->rip,
        frame->r8, frame->r9, frame->r10,
        frame->r11, frame->r12, frame->r13,
        frame->r14, frame->r15, frame->rflags,
        frame->ss, frame->ds, frame->cs,
        getCR0(), getCR2(), frame->cr3,
        getCR4(), Core_GetIrql(), getEFER()
    );
}


void Arch_InstallExceptionHandlers()
{
    Arch_RawRegisterInterrupt(0x00, (uintptr_t)Arch_FPEHandler);
    Arch_RawRegisterInterrupt(0x01, (uintptr_t)Arch_DbgExceptHandler);
    Arch_RawRegisterInterrupt(0x03, (uintptr_t)Arch_DbgExceptHandler);
    Arch_RawRegisterInterrupt(0x06, (uintptr_t)Arch_UndefinedOpcodeHandler);
    Arch_RawRegisterInterrupt(0x08, (uintptr_t)Arch_DoubleFaultHandler);
    Arch_RawRegisterInterrupt(0x0b, (uintptr_t)Arch_SegvHandler);
    Arch_RawRegisterInterrupt(0x0c, (uintptr_t)Arch_SegvHandler);
    Arch_RawRegisterInterrupt(0x0d, (uintptr_t)Arch_SegvHandler);
    Arch_RawRegisterInterrupt(0x0e, (uintptr_t)Arch_PageFaultHandler);
    Arch_RawRegisterInterrupt(0x10, (uintptr_t)Arch_FPEHandler);
    Arch_RawRegisterInterrupt(0x11, (uintptr_t)Arch_SegvHandler);
    Arch_RawRegisterInterrupt(0x13, (uintptr_t)Arch_FPEHandler);
}
