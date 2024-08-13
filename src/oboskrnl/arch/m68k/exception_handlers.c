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
        page what = {.addr=frame->format_7.fa & ~0xfff};
        page* page = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        if (page && page->prot.present)
            mm_ec |= PF_EC_PRESENT;
        obos_status status = Mm_HandlePageFault(CoreS_GetCPULocalPtr()->currentContext, frame->format_7.fa & ~0xfff, mm_ec);
        switch (status)
        {
            case OBOS_STATUS_SUCCESS:
                return;
            case OBOS_STATUS_UNHANDLED:
                break;
            default:
            {
                static const char format[] = "Handling page fault with error code 0x%x on address %p failed.\n";
                OBOS_Warning(format, mm_ec, frame->format_7.fa);
                break;
            }
        }
    }
    OBOS_Panic(OBOS_PANIC_EXCEPTION,
        "Access fault in %s-mode at 0x%p while trying to %s the %spresent page at 0x%p.\nRegister dump:\n"
        "d0: 0x%p, d1: 0x%p, d2: 0x%p, d3: 0x%p\n"
        "d1: 0x%p, d5: 0x%p, d6: 0x%p, d7: 0x%p\n"
        "a0: 0x%p, a1: 0x%p, a2: 0x%p, a3: 0x%p\n"
        "a4: 0x%p, a5: 0x%p, a6: 0x%p, sp: 0x%p\n"
        "pc: 0x%p, sr: 0x%p\n",
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