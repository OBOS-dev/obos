/*
 * oboskrnl/arch/x86_64/gdbstub/debug.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "allocators/base.h"
#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/lapic.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/debug.h>
#include <arch/x86_64/gdbstub/packet_dispatcher.h>
#include <arch/x86_64/gdbstub/alloc.h>

#include <irq/irql.h>

#include <locks/mutex.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/process.h>

gdb_connection* Kdbg_CurrentConnection;

bool Kdbg_Paused = false;

#define get_dbg_ctx() (CoreS_GetCPULocalPtr()->arch_specific.dbg_ctx)

void Kdbg_Break()
{
    asm("int3");
}
static mutex lock;
void Kdbg_CallDebugExceptionHandler(interrupt_frame* frame, bool isSource)
{
    if (!Kdbg_CurrentConnection)
        return;
    if (isSource)
    {
        Kdbg_Paused = true;
        ipi_lapic_info lapic = {
            .isShorthand=true,
            .info.shorthand = LAPIC_DESTINATION_SHORTHAND_ALL_BUT_SELF,
        };
        ipi_vector_info vec = {
            .deliveryMode = LAPIC_DELIVERY_MODE_NMI,
        };
        Arch_LAPICSendIPI(lapic, vec);
    }
    get_dbg_ctx().interrupted_thread = Core_GetCurrentThread();
    get_dbg_ctx().interrupted_thread->flags |= THREAD_FLAGS_DEBUGGER_BLOCKED;
    get_dbg_ctx().interrupt_ctx.cr3 = frame->cr3;
    get_dbg_ctx().interrupt_ctx.irql = 0;
    memcpy(&get_dbg_ctx().interrupt_ctx.frame, frame, sizeof(*frame));
    get_dbg_ctx().interrupt_ctx.gs_base = frame->cs & 0x3 ? rdmsr(0xc0000102) : (uintptr_t)CoreS_GetCPULocalPtr();
    get_dbg_ctx().interrupt_ctx.fs_base = rdmsr(0xc0000100);
    Kdbg_GeneralDebugExceptionHandler(Kdbg_CurrentConnection, &get_dbg_ctx(), isSource);
    memcpy(frame, &get_dbg_ctx().interrupt_ctx.frame, sizeof(*frame));
}
void Kdbg_NotifyGDB(gdb_connection* con, uint8_t signal)
{
    char* packet = KdbgH_FormatResponse("T%02xthread:p%x.%x;", signal, Core_GetCurrentThread()->proc->pid+1, Core_GetCurrentThread()->tid);
    Kdbg_ConnectionSendPacket(con, packet);
}

void Kdbg_int3_handler(interrupt_frame* frame)
{
    asm("sti");
    static bool initialized = false;
    obos_status status = Core_MutexTryAcquire(&lock);
    if (obos_is_error(status))
        return;
    if (initialized)
        Kdbg_NotifyGDB(Kdbg_CurrentConnection, 0x05); // trap
    // Adjust rip properly
    uint8_t offset = 0;
    if (*(uint8_t*)(frame->rip-1) == 0xcc /*int3*/)
        offset = 1; // ... int3 (0xcc xx)
    if (*(uint8_t*)(frame->rip-1) == 0xcd)
        offset = 2; // ... int 3 (0xcd, 0x03, xx)
    uintptr_t old_rip = frame->rip;
    frame->rip -= offset;
    sw_breakpoint *bp = nullptr;
    for (sw_breakpoint* node = LIST_GET_HEAD(sw_breakpoint_list, &Kdbg_CurrentConnection->sw_breakpoints); node;)
    {
        if (node->addr == frame->rip)
        {
            bp = node;
            break;
        }

        node = LIST_GET_NEXT(sw_breakpoint_list, &Kdbg_CurrentConnection->sw_breakpoints, node);
    }
    Kdbg_CallDebugExceptionHandler(frame, true);
    if ((old_rip - offset) == frame->rip)
    {
        if (!bp)
            frame->rip = old_rip; // if the rip wasn't artifically modified, set rip back to the instruction it was before
        // else
        //     frame->rip = frame->rip; // if the rip wasn't artifically modified, set rip back to the instruction it was before
    }
    Core_MutexRelease(&lock);
    if (!initialized)
        Kdbg_CurrentConnection->connection_active = true;
    initialized = Kdbg_CurrentConnection->connection_active;
    asm("cli");
}
void Kdbg_int1_handler(interrupt_frame* frame)
{
    asm("sti");
    Core_MutexAcquire(&lock);
    if (getDR6() & BIT(14))
    {
        Kdbg_NotifyGDB(Kdbg_CurrentConnection, 0x05); // trap
        frame->rflags &= ~RFLAGS_TRAP;
    }
    Kdbg_CallDebugExceptionHandler(frame, true);
    Core_MutexRelease(&lock);
    asm("cli");
}

void Kdbg_GeneralDebugExceptionHandler(gdb_connection* conn, gdb_ctx* dbg_ctx, bool isSource)
{
    if (!isSource)
    {
        while (Kdbg_Paused && !dbg_ctx->wake)
            CoreH_DispatchDPCs();
        return;
    }
    while(dbg_ctx->interrupted_thread->flags & THREAD_FLAGS_DEBUGGER_BLOCKED && Kdbg_CurrentConnection->connection_active)
	{
		char* packet = nullptr;
		size_t packetLen = 0;
		Kdbg_ConnectionRecvPacket(conn, &packet, &packetLen);
        // printf("<%s\n", packet);
		obos_status st = Kdbg_DispatchPacket(conn, packet, packetLen, dbg_ctx);
		if (obos_is_error(st))
            OBOS_Debug("Kdbg: While dispatching packet: Got status %d\n", st);
        Kdbg_Free(packet);
    }
    Kdbg_Paused = false;
}