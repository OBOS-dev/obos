/*
 * oboskrnl/arch/x86_64/gdbstub/bp.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/breakpoint.h>
#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/bp.h>

#include <arch/x86_64/pmm.h>

#include <mm/bare_map.h>
#include <mm/page.h>
#include <mm/alloc.h>
#include <mm/context.h>

#include <stdint.h>
#include <utils/tree.h>
#include <utils/list.h>

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

LIST_GENERATE(sw_breakpoint_list, struct sw_breakpoint, node);

struct args
{
    uintptr_t address;
    uintptr_t kind;
};

static struct args parse_arguments(const char* arguments, size_t argumentsLen)
{
    struct args ret = {};
    size_t addrLen = strchr(arguments, ',')-1;
    size_t kindLen = strchr(arguments+addrLen, ';');
    if (kindLen != argumentsLen)
        kindLen--;
    ret.address = KdbgH_hex2bin(arguments, addrLen);
    ret.kind = KdbgH_hex2bin(arguments+addrLen+1, kindLen);
    return ret;
}

#define X86_INT3 0xCC

// Adds a software breakpoint.
obos_status Kdbg_GDB_Z0(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_USERDATA;
    struct args args = parse_arguments(arguments, argumentsLen);
    char* response = nullptr;
    sw_breakpoint *bp = Kdbg_Malloc(sizeof(*bp));
    bp->addr = args.address;
    page what = {.addr=bp->addr&~0xfff};
    bool retried = false;
    retry:
    page* found = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
    if (!found && !retried)
    {
        retried = true;
        what.addr &= ~0x1fffff;
        goto retry;
    }
    if (!found)
    {
        response = "E.No such breakpoint at address";
        goto respond;
    }
    prot_flags prot = 0, oldProt = 0;
    if (found->prot.executable)
        oldProt |= OBOS_PROTECTION_EXECUTABLE;
    if (!(found->prot.rw))
        oldProt |= OBOS_PROTECTION_READ_ONLY;
    if (found->prot.user)
        oldProt |= OBOS_PROTECTION_USER_PAGE;
    prot = oldProt & ~OBOS_PROTECTION_READ_ONLY;
    Mm_VirtualMemoryProtect(&Mm_KernelContext, (void*)what.addr, found->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, prot, 2);
    bp->at = *(uint8_t*)bp->addr;
    *(uint8_t*)bp->addr = X86_INT3;
    Mm_VirtualMemoryProtect(&Mm_KernelContext, (void*)what.addr, found->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, oldProt, 2);
    LIST_APPEND(sw_breakpoint_list, &con->sw_breakpoints, bp);
    response = "OK";
    respond:
    (void)0;
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    return status;
}
// Removes a software breakpoint.
obos_status Kdbg_GDB_z0(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_USERDATA;
    struct args args = parse_arguments(arguments, argumentsLen);
    char* response = nullptr;
    sw_breakpoint *bp = nullptr;
    for (sw_breakpoint* node = LIST_GET_HEAD(sw_breakpoint_list, &con->sw_breakpoints); node;)
    {
        if (node->addr == args.address)
        {
            bp = node;
            break;
        }

        node = LIST_GET_NEXT(sw_breakpoint_list, &con->sw_breakpoints, node);
    }
    if (!bp)
    {
        response = "E.No such breakpoint at address";
        goto respond;
    }
    page what = {.addr=bp->addr&~0xfff};
    bool retried = false;
    retry:
    (void)0;
    page* found = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
    if (!found && !retried)
    {
        retried = true;
        what.addr &= ~0x1fffff;
        goto retry;
    }
    if (!found)
    {
        response = "E.No such breakpoint at address";
        goto respond;
    }
    prot_flags prot = 0, oldProt = 0;
    if (found->prot.executable)
        oldProt |= OBOS_PROTECTION_EXECUTABLE;
    if (!(found->prot.rw))
        oldProt |= OBOS_PROTECTION_READ_ONLY;
    if (found->prot.user)
        oldProt |= OBOS_PROTECTION_USER_PAGE;
    prot = oldProt &= ~OBOS_PROTECTION_READ_ONLY;
    Mm_VirtualMemoryProtect(&Mm_KernelContext, (void*)what.addr, found->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, prot, 2);
    *(uint8_t*)bp->addr = bp->at;
    Mm_VirtualMemoryProtect(&Mm_KernelContext, (void*)what.addr, found->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE, oldProt, 2);
    LIST_APPEND(sw_breakpoint_list, &con->sw_breakpoints, bp);
    response = "OK";
    respond:
    (void)0;
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    return status;
}