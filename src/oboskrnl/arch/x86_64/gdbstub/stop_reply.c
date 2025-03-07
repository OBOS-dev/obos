/*
 * oboskrnl/arch/x86_64/gdbstub/stop_reply.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "arch/x86_64/pmm.h"
#include "power/shutdown.h"
#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/breakpoint.h>
#include <arch/x86_64/gdbstub/debug.h>

#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/lapic.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>

#include <mm/page.h>
#include <mm/context.h>

#include <stdint.h>
#include <uacpi/sleep.h>

#include <irq/irq.h>

#include <utils/tree.h>

#define NO_ARGUMENTS \
do {\
    OBOS_UNUSED(arguments);\
    OBOS_UNUSED(argumentsLen);\
} while(0)
#define NO_RESPONSE \
do {\
    OBOS_UNUSED(con);\
} while(0)
#define NO_USERDATA \
do {\
    OBOS_UNUSED(userdata);\
} while(0)
#define NO_CTX \
do {\
    OBOS_UNUSED(dbg_ctx);\
} while(0)
// 0xffffffff means all
// 0xfffffffe means current
static size_t parse_gdb_thread_id(const char* id, size_t len, uint32_t* pid, uint32_t* tid);

// '?' packet
// Queries the stop reason.
obos_status Kdbg_GDB_query_halt(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_USERDATA;
    NO_CTX;
    NO_ARGUMENTS;
    char* response = KdbgH_FormatResponse("T05thread:p%x.%x;%s", 
        (dbg_ctx->interrupted_thread->proc->pid+1) >= 256 ? __builtin_bswap32(dbg_ctx->interrupted_thread->proc->pid + 1) : dbg_ctx->interrupted_thread->proc->pid + 1,
        dbg_ctx->interrupted_thread->tid >= 256 ? __builtin_bswap32(dbg_ctx->interrupted_thread->tid) : dbg_ctx->interrupted_thread->tid, 
        con->gdb_supported & BIT(0) ?
            ";swbreak:;" : ""
    );
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    Kdbg_Free(response);
    return status;
}
thread* current_g_thread = nullptr;
thread* current_c_thread = nullptr;
bool c_all_threads = false;
// static void strreverse(char* begin, int size)
// {
// 	int i = 0;
// 	char tmp = 0;

// 	for (i = 0; i < size / 2; i++)
// 	{
// 		tmp = begin[i];
// 		begin[i] = begin[size - i - 1];
// 		begin[size - i - 1] = tmp;
// 	}
// }
// Credit: http://www.strudel.org.uk/itoa/
// static char* itoa_unsigned(uintptr_t value, char* result, int base)
// {
// 	// check that the base if valid

// 	if (base < 2 || base > 16)
// 	{
// 		*result = 0;
// 		return result;
// 	}

// 	char* out = result;

// 	uintptr_t quotient = value;

// 	do
// 	{

// 		uintptr_t abs = /*(quotient % base) < 0 ? (-(quotient % base)) : */(quotient % base);

// 		*out = "0123456789abcdef"[abs];

// 		++out;

// 		quotient /= base;

// 	} while (quotient);

// 	strreverse(result, out - result);

// 	return result;

// }
static void format_register64(char** str, uintptr_t reg)
{
    reg = __builtin_bswap64(reg);
    (*str) += snprintf(*str, 17, "%016lx", reg);
}
static void format_register32(char** str, uintptr_t reg)
{
    reg = __builtin_bswap32(reg);
    (*str) += snprintf(*str, 9, "%08x", reg);
}
// Read Registers
obos_status Kdbg_GDB_g(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata)
{
    NO_USERDATA;
    OBOS_UNUSED(ctx);
    NO_ARGUMENTS;
    // This'll be horrid....
    // const char* format = 
    // // rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp
    // "%016lx%016lx%016lx%016lx%016lx%016lx%016lx%016lx" // that's 8
    // // r8-r15
    // "%016lx%016lx%016lx%016lx%016lx%016lx%016lx%016lx" // that's 8
    // // rip, eflags, cc, ss, ds, es, fs, gs
    // "%016lx%08x%08x%08x%08x%08x%08x%08x"; // that's 8
    // st0-st7 (ignore, set to zero)
    // "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    // // a bunch of fpu ctrl registers
    // "0000000000000000000000000000000000000000000000000000000000000000"
    if (!current_g_thread || !current_g_thread->masterCPU)
        current_g_thread = Core_GetCurrentThread();
    gdb_ctx dbg_ctx = current_g_thread->masterCPU->arch_specific.dbg_ctx;
    if (current_g_thread->masterCPU == CoreS_GetCPULocalPtr() && current_g_thread != Core_GetCurrentThread())
        dbg_ctx.interrupt_ctx = current_g_thread->context;
    interrupt_frame* frame = &dbg_ctx.interrupt_ctx.frame;
    // char* response = KdbgH_FormatResponseSized(320, format,
    //     __builtin_bswap64(frame->rax),__builtin_bswap64(frame->rbx),__builtin_bswap64(frame->rcx),__builtin_bswap64(frame->rdx),__builtin_bswap64(frame->rsi),__builtin_bswap64(frame->rdi),__builtin_bswap64(frame->rbp),__builtin_bswap64(frame->rsp),
    //     __builtin_bswap64(frame->r8) ,__builtin_bswap64(frame->r9) ,__builtin_bswap64(frame->r10),__builtin_bswap64(frame->r11),__builtin_bswap64(frame->r12),__builtin_bswap64(frame->r13),__builtin_bswap64(frame->r14),__builtin_bswap64(frame->r15),
    //     __builtin_bswap64(frame->rip),__builtin_bswap32(frame->rflags),__builtin_bswap32(frame->cs) ,__builtin_bswap32(frame->ss) ,__builtin_bswap32(frame->ds) , 0,0,0
    // );
    char* response = Kdbg_Calloc(329, sizeof(char));
    char* iter = response;
    format_register64(&iter, frame->rax);
    format_register64(&iter, frame->rbx);
    format_register64(&iter, frame->rcx);
    format_register64(&iter, frame->rdx);
    format_register64(&iter, frame->rsi);
    format_register64(&iter, frame->rdi);
    format_register64(&iter, frame->rbp);
    format_register64(&iter, frame->rsp);
    format_register64(&iter, frame->r8);
    format_register64(&iter, frame->r9);
    format_register64(&iter, frame->r10);
    format_register64(&iter, frame->r11);
    format_register64(&iter, frame->r12);
    format_register64(&iter, frame->r13);
    format_register64(&iter, frame->r14);
    format_register64(&iter, frame->r15);
    format_register64(&iter, frame->rip);
    format_register32(&iter, frame->rflags);
    format_register32(&iter, frame->cs);
    format_register32(&iter, frame->ss);
    format_register32(&iter, frame->ds);
    format_register32(&iter, 0);
    format_register32(&iter, 0);
    format_register32(&iter, 0);
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    Kdbg_Free(response);
    return status;
}
static uintptr_t get_register64(const char** str)
{
    uintptr_t reg = KdbgH_hex2bin(*str, 16);
    (*str) += 16;
    return __builtin_bswap64(reg);
}
static uintptr_t get_register32(const char** str)
{
    uintptr_t reg = KdbgH_hex2bin(*str, 8);
    (*str) += 8;
    return __builtin_bswap32(reg & 0xffffffff);
}
// Write Registers
obos_status Kdbg_GDB_G(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata)
{
    NO_USERDATA;
    OBOS_UNUSED(argumentsLen);
    OBOS_UNUSED(ctx);
    if (!current_g_thread || !current_g_thread->masterCPU)
        current_g_thread = Core_GetCurrentThread();
    gdb_ctx *dbg_ctx = &current_g_thread->masterCPU->arch_specific.dbg_ctx;
    interrupt_frame* frame = &dbg_ctx->interrupt_ctx.frame;
    if (current_g_thread->masterCPU == CoreS_GetCPULocalPtr() && current_g_thread != Core_GetCurrentThread())
        frame = &current_g_thread->context.frame;
    // rax,rbx,rcx,rdx,rsi,rdi,rbp,rsp
    // r8-r15
    // rip, eflags, cc, ss, ds, es, fs, gs
    const char* iter = arguments;
    frame->rax = get_register64(&iter);
    frame->rbx = get_register64(&iter);
    frame->rcx = get_register64(&iter);
    frame->rdx = get_register64(&iter);
    frame->rsi = get_register64(&iter);
    frame->rdi = get_register64(&iter);
    frame->rbp = get_register64(&iter);
    frame->rsp = get_register64(&iter);
    frame->r8  = get_register64(&iter);
    frame->r9  = get_register64(&iter);
    frame->r10 = get_register64(&iter);
    frame->r11 = get_register64(&iter);
    frame->r12 = get_register64(&iter);
    frame->r13 = get_register64(&iter);
    frame->r14 = get_register64(&iter);
    frame->r15 = get_register64(&iter);
    frame->rip = get_register64(&iter);
    frame->rflags = get_register32(&iter);
    frame->cs = get_register32(&iter);
    frame->ss = get_register32(&iter);
    frame->cs = get_register32(&iter);
    get_register32(&iter);
    get_register32(&iter);
    get_register32(&iter);
    return Kdbg_ConnectionSendPacket(con, "OK");
}
// Kill the kernel (shutdown)
obos_status Kdbg_GDB_k(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_RESPONSE;
    NO_ARGUMENTS;
    NO_USERDATA;
    // OBOS_Log("%s: GDB sent kill command. Requesting system shutdown...\n", __func__);
    OBOS_Shutdown();
    OBOS_UNREACHABLE;
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_D(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_RESPONSE;
    NO_ARGUMENTS;
    NO_USERDATA;
    Kdbg_Paused = false;
    Kdbg_CurrentConnection->connection_active = false;
    Kdbg_CurrentConnection->gdb_supported = 0;
    Kdbg_CurrentConnection->flags = 0;
    return Kdbg_ConnectionSendPacket(con, "OK");
}
// Read Memory
OBOS_NO_KASAN obos_status Kdbg_GDB_m(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_USERDATA;
    NO_CTX;
    size_t addrLen = strnchr(arguments, ',', argumentsLen)-1;
    if (addrLen > 16)
        return Kdbg_ConnectionSendPacket(con, "");
    uintptr_t address = KdbgH_hex2bin(arguments, addrLen);
    if ((argumentsLen-addrLen-1) > 16)
        return Kdbg_ConnectionSendPacket(con, "");
    size_t memoryLen = KdbgH_hex2bin(arguments+addrLen+1, argumentsLen-addrLen-1);
    // printf("GDB requested %d bytes of memory at %p\n", memoryLen, address);
    uintptr_t top = address + memoryLen;
    // Fun fact!
    // This buffer takes more bytes than the actual memory takes!
    char* response = Kdbg_Calloc(memoryLen*2+1, sizeof(char));
    size_t i = 0;
    size_t nRead = 0;
    static const char hexmask[16] = "0123456789abcdef";
    uintptr_t curr_phys = 0;
    uintptr_t last_virt = 0;
    for (uintptr_t addr = address; addr < top && nRead < memoryLen; addr++, i += 2, nRead++)
    {
        if ((addr & ~0xfff) != last_virt)
            MmS_QueryPageInfo(Mm_KernelContext.pt, addr & ~0xfff, nullptr, &curr_phys);
        if (!curr_phys)
            break;
        uint8_t byte = *(uint8_t*)Arch_MapToHHDM(curr_phys + (addr & 0xfff));
        response[i+1] = hexmask[byte&0xf];
        response[i] = hexmask[byte>>4];
        // printf("%d\n", i);
        last_virt = addr & ~0xfff;
    }
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    Kdbg_Free(response);
    return status;
}
// Write Memory
obos_status Kdbg_GDB_M(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_USERDATA;
    NO_CTX;
    OBOS_UNUSED(argumentsLen);
    size_t addrLen = strnchr(arguments, ',', argumentsLen)-1;
    if (addrLen > 16)
        return Kdbg_ConnectionSendPacket(con, "E.Invalid address.");
    uintptr_t address = KdbgH_hex2bin(arguments, addrLen);
    size_t argv2_len = strnchr(arguments+addrLen+1, ':', argumentsLen-addrLen-1)-1;
    if (argv2_len > 16)
        return Kdbg_ConnectionSendPacket(con, "E.Invalid size.");
    size_t memoryLen = KdbgH_hex2bin(arguments+addrLen+1, argumentsLen-addrLen-1);
    uintptr_t top = address + memoryLen;
    size_t i = 0;
    size_t nRead = 0;
    // static const char hexmask[16] = "0123456789abcdef";
    const char* iter = arguments+strnchr(arguments, ':', argumentsLen);
    uintptr_t curr_phys = 0;
    uintptr_t last_virt = address & ~0xfff;
    const char* response = "OK";
    for (uintptr_t addr = address; addr < top && nRead < memoryLen; addr++, i += 2, nRead++, iter += 2)
    {
        if ((addr & ~0xfff) != last_virt)
            MmS_QueryPageInfo(Mm_KernelContext.pt, addr & ~0xfff, nullptr, &curr_phys);
        if (!curr_phys)
        {
            // TODO: Check this before hand?
            response = "E.Memory not mapped.";
            break;
        }
        *(uint8_t*)Arch_MapToHHDM(curr_phys + (addr & 0xfff)) = KdbgH_hex2bin(iter, 2);
    }
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    return status;
}
// Step
obos_status Kdbg_GDB_c(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_CTX;
    NO_USERDATA;
    if ((!current_c_thread || !current_c_thread->masterCPU) && !c_all_threads)
        current_c_thread = Core_GetCurrentThread();
    if (!c_all_threads)
    {
        current_c_thread->flags &= ~THREAD_FLAGS_DEBUGGER_BLOCKED;
        current_c_thread->masterCPU->arch_specific.dbg_ctx.interrupt_ctx.frame.rflags &= ~RFLAGS_TRAP;
    }
    else 
    {
        for (size_t i = 0; i < Core_CpuCount; i++)
        {
            Core_CpuInfo[i].arch_specific.dbg_ctx.interrupted_thread->flags &= ~THREAD_FLAGS_DEBUGGER_BLOCKED;
            Core_CpuInfo[i].arch_specific.dbg_ctx.interrupt_ctx.frame.rflags &= ~RFLAGS_TRAP;
        }
        // Kdbg_Paused = false;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_C(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    return Kdbg_GDB_c(con, arguments, argumentsLen, dbg_ctx, userdata);
}
obos_status Kdbg_GDB_s(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_CTX;
    NO_USERDATA;
    // TODO: Support arguments.
    // c_all_threads = false;
    if ((!current_c_thread || !current_c_thread->masterCPU) && !c_all_threads)
        current_c_thread = Core_GetCurrentThread();
    // FIXME: Stepping all threads breaks everything
    if (!c_all_threads)
    {
        current_c_thread->flags &= ~THREAD_FLAGS_DEBUGGER_BLOCKED;
        current_c_thread->masterCPU->arch_specific.dbg_ctx.interrupt_ctx.frame.rflags |= RFLAGS_TRAP;
    }
    else 
    {
        for (size_t i = 0; i < Core_CpuCount; i++)
        {
            Core_CpuInfo[i].arch_specific.dbg_ctx.interrupt_ctx.frame.rflags |= RFLAGS_TRAP;
            Core_CpuInfo[i].arch_specific.dbg_ctx.interrupted_thread->flags &= ~THREAD_FLAGS_DEBUGGER_BLOCKED;
        }
    }
    // Kdbg_Paused = false;
    return OBOS_STATUS_SUCCESS;
}
// Query thread status.
obos_status Kdbg_GDB_T(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_USERDATA;
    NO_CTX;
    NO_ARGUMENTS;
    uint32_t tid = 0;
    uint32_t pid = 0; // ignored
    parse_gdb_thread_id(arguments, argumentsLen, &pid, &tid);
    if (pid == 0xfffffffe)
        pid = Core_GetCurrentThread()->proc->pid+1;
    thread* thr = nullptr;
    if (tid == 0xfffffffe)
        thr = Core_GetCurrentThread();
    else if (tid == (uint32_t)-1)
    {
        // TODO: Implement.
        return Kdbg_ConnectionSendPacket(con, "E.Could not find thread");
    }
    else
    {
        for(thread_node* node = OBOS_KernelProcess->threads.head; node && !thr; )
        {
            if (node->data->tid == tid)
                thr = node->data;
            node = node->next;
        }
    }
    if (!thr)
        return Kdbg_ConnectionSendPacket(con, "");
    if (thr->flags & THREAD_FLAGS_DIED)
        return Kdbg_ConnectionSendPacket(con, "");
    return Kdbg_ConnectionSendPacket(con, "OK");
}
// These packets: g,G,k,m,M
// But multithreaded
obos_status Kdbg_GDB_H(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_USERDATA;
    // Forward the arguments.
    OBOS_ASSERT(argumentsLen);
    uint32_t tid = 0;
    uint32_t pid = 0; // ignored
    parse_gdb_thread_id(arguments+1, argumentsLen-1, &pid, &tid);
    if (pid == 0xfffffffe)
        pid = Core_GetCurrentThread()->proc->pid+1;
    thread** pThr = arguments[0] == 'c' ? &current_c_thread : &current_g_thread;
    if (tid == 0xfffffffe)
        *pThr = Core_GetCurrentThread();
    else if (tid == (uint32_t)-1)
    {
        // TODO: Implement g argument for all.
        if (arguments[0] == 'c')
        {
            c_all_threads = true;
            return Kdbg_ConnectionSendPacket(con, "OK");
        }
        else 
        {
            return Kdbg_ConnectionSendPacket(con, "E.Unsupported");
        }
    }
    else
    {
        for(thread_node* node = OBOS_KernelProcess->threads.head; node; )
        {
            if (node->data->tid == tid)
            {
                *pThr = node->data;
                break;
            }
            node = node->next;
        }
    }
    if (*pThr)
        return Kdbg_ConnectionSendPacket(con, "OK");
    return Kdbg_ConnectionSendPacket(con, "E.Could not find thread");
}

static size_t parse_gdb_thread_id(const char* id, size_t len, uint32_t* pid, uint32_t* tid)
{
    if (!len || !pid || !tid)
        return 0;
    bool hasPid = *id == 'p';
    // Minimum contents of a thread id:
    // 0
    // With a pid:
    // p0.0
    if (hasPid && len < 4)
        return 0;
    size_t idSize = 0;
    if (hasPid)
    {
        id++;
        size_t pidLen = strnchr(id, '.', len-1)-1;
        idSize += pidLen + 1;
        if (id[0] == '-' && id[1] == '1')
            *pid = 0xffffffff;
        else
            *pid = KdbgH_hex2bin(id, pidLen);
        if (!(*pid))
            *pid = 0xfffffffe;
        id += pidLen + 1;
    }
    if (!(*id))
        return 0;
    size_t tidLen = strnchr(id, ';', len);
    idSize += tidLen;
    if (id[0] == '-' && id[1] == '1')
        *tid = 0xffffffff;
    else
        *tid = KdbgH_hex2bin(id, tidLen);
    if (!(*tid))
        *tid = 0xfffffffe;
    return tidLen;
}