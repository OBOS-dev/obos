/*
 * oboskrnl/arch/m68k/exception_handlers.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <mm/init.h>
#include <mm/handler.h>

#include <scheduler/cpu_local.h>

#include <arch/m68k/interrupt_frame.h>

// well technically it's an access fault but whatever
void Arch_PageFaultHandler(interrupt_frame* frame)
{
    uint32_t mm_ec = 0;
    if (frame->format_7.ssw.atc)
        mm_ec &= ~PF_EC_PRESENT;
    if (!frame->format_7.ssw.rw)
        mm_ec |= PF_EC_RW;
    if (!(frame->sr & (1<<13) /* Supervisor */))
        mm_ec |= PF_EC_UM;
    if (Mm_IsInitialized())
    {
        mm_ec &= ~PF_EC_PRESENT;
        page_info curr = {};
        context* ctx = CoreS_GetCPULocalPtr()->currentContext;
        MmS_QueryPageInfo(ctx->pt, frame->format_7.fa & ~0xfff, &curr, nullptr);
        if (curr.prot.present)
            mm_ec |= PF_EC_PRESENT;
        obos_status status = Mm_HandlePageFault(ctx, frame->format_7.fa & ~0xfff, mm_ec);
        switch (status)
        {
            case OBOS_STATUS_SUCCESS:
                return;
            case OBOS_STATUS_UNHANDLED:
                break;
            default:
            {
                static const char format[] = "Handling page fault with error code 0x%x on address %08x failed.\n";
                OBOS_Warning(format, mm_ec, frame->format_7.fa);
                break;
            }
        }
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION,
        "Access fault in %s-mode at 0x%08x while trying to %s the %spresent page at 0x%08x.\nRegister dump:\n"
        "d0: 0x%08x, d1: 0x%08x, d2: 0x%08x, d3: 0x%08x\n"
        "d1: 0x%08x, d5: 0x%08x, d6: 0x%08x, d7: 0x%08x\n"
        "a0: 0x%08x, a1: 0x%08x, a2: 0x%08x, a3: 0x%08x\n"
        "a4: 0x%08x, a5: 0x%08x, a6: 0x%08x, sp: 0x%08x\n"
        "pc: 0x%08x, sr: 0x%08x\n",
        (mm_ec & PF_EC_UM) ? "user" : "kernel",
        frame->pc,
        (mm_ec & PF_EC_RW) ? "write" : "read",
        (mm_ec & PF_EC_PRESENT) ? "" : "non-",
        frame->format_7.fa,
        frame->d0, frame->d1, frame->d2, frame->d3,
        frame->d4, frame->d5, frame->d6, frame->d7,
        frame->a0, frame->a1, frame->a2, frame->a3,
        frame->a4, frame->a5, frame->a6, frame->usp,
        frame->pc, frame->sr
    );
}