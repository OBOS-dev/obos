/*
 * oboskrnl/arch/x86_64/gdbstub/debug.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "irq/irql.h"
#include <int.h>
#include <klog.h>
#include <error.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/debug.h>
#include <arch/x86_64/gdbstub/packet_dispatcher.h>
#include <arch/x86_64/gdbstub/alloc.h>

#include <irq/dpc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>

gdb_connection* Kdbg_CurrentConnection;

#define get_dbg_ctx() (CoreS_GetCPULocalPtr()->arch_specific.dbg_ctx)

static void dpc_handler(dpc* obj, void* udata)
{
    Kdbg_GeneralDebugExceptionHandler(obj, Kdbg_CurrentConnection, udata);
}
static void general_dbg_except(interrupt_frame* frame)
{
    asm("sti");
    get_dbg_ctx().interrupted_thread = Core_GetCurrentThread();
    get_dbg_ctx().interrupt_ctx.cr3 = getCR3();
    get_dbg_ctx().interrupt_ctx.irql = Core_GetIrql();
    get_dbg_ctx().interrupt_ctx.frame = *frame;
    get_dbg_ctx().interrupt_ctx.gs_base = (uintptr_t)CoreS_GetCPULocalPtr();
    get_dbg_ctx().interrupt_ctx.fs_base = 0;
    dpc* work = CoreH_AllocateDPC(nullptr);
    if (__builtin_expect(work == nullptr, 0))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a dpc!\n");
    work->userdata = &get_dbg_ctx();
    irql oldIrql = Core_RaiseIrqlNoThread(IRQL_DISPATCH);
    CoreH_ThreadBlock(Core_GetCurrentThread(), false);
    CoreH_InitializeDPC(work, dpc_handler, Core_DefaultThreadAffinity);
    Core_LowerIrqlNoDPCDispatch(oldIrql);
    Core_Yield();
    asm("cli");
}
void Kdbg_int3_handler(interrupt_frame* frame)
{
    general_dbg_except(frame);
}
void Kdbg_int1_handler(interrupt_frame* frame)
{
    general_dbg_except(frame);
}

void Kdbg_GeneralDebugExceptionHandler(dpc* obj, gdb_connection* conn, gdb_ctx* dbg_ctx)
{
    while(dbg_ctx->interrupted_thread->status == THREAD_STATUS_BLOCKED)
	{
		char* packet = nullptr;
		size_t packetLen = 0;
		Kdbg_ConnectionRecvPacket(conn, &packet, &packetLen);
		Kdbg_DispatchPacket(conn, packet, packetLen, dbg_ctx);
		Kdbg_Free(packet);
	}
    CoreH_FreeDPC(obj);
}