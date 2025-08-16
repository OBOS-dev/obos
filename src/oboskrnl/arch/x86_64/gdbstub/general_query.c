/*
 * oboskrnl/arch/x86_64/gdbstub/general_query.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <cmdline.h>
#include <memmanip.h>
#include <cmdline.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>

#include <allocators/base.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/general_query.h>

#include <arch/x86_64/asm_helpers.h>

#include <uacpi_libc.h>

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

obos_status Kdbg_GDB_qC(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_USERDATA;
    uint32_t tid = dbg_ctx->interrupted_thread->tid > 255 ?  __builtin_bswap32(dbg_ctx->interrupted_thread->tid+1) : dbg_ctx->interrupted_thread->tid;
    uint32_t pid = (dbg_ctx->interrupted_thread->proc->pid+1) > 255 ? __builtin_bswap32(dbg_ctx->interrupted_thread->proc->pid+1) : dbg_ctx->interrupted_thread->proc->pid+1;
    char* response = KdbgH_FormatResponse("QCp%x.%x", pid, tid);
    Kdbg_ConnectionSendPacket(con, response);
    Kdbg_Free(response);
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_q_ThreadInfo(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_USERDATA;
    NO_CTX;
    char* response = nullptr;
    bool shouldFreeResponse = true;
    retry:
    if (!con->q_ThreadInfo_ctx.last_thread && con->q_ThreadInfo_ctx.received_first)
    {
        // Respond with an l.
        response = "l";
        shouldFreeResponse = false;
        con->q_ThreadInfo_ctx.received_first = false;
        goto respond;
    }
    if (!con->q_ThreadInfo_ctx.received_first)
    {
        con->q_ThreadInfo_ctx.received_first = true;
        con->q_ThreadInfo_ctx.last_thread = OBOS_KernelProcess->threads.head;
    }
    while (con->q_ThreadInfo_ctx.last_thread && (con->q_ThreadInfo_ctx.last_thread->data->flags & THREAD_FLAGS_DIED))
        con->q_ThreadInfo_ctx.last_thread = con->q_ThreadInfo_ctx.last_thread->next;
    if (!con->q_ThreadInfo_ctx.last_thread)
        goto retry;
    response = KdbgH_FormatResponse("mp01.%x", con->q_ThreadInfo_ctx.last_thread->data->tid);
    con->q_ThreadInfo_ctx.last_thread = con->q_ThreadInfo_ctx.last_thread->next;  
    respond:
    (void)0;
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    if (shouldFreeResponse)
        Kdbg_Free(response);
    return status;
}
obos_status Kdbg_GDB_QStartNoAckMode(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_USERDATA;
    NO_CTX;
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_qSupported(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_RESPONSE;
    NO_USERDATA;
    NO_CTX;
    OBOS_UNUSED(argumentsLen);
    // We support:
    // swbreak
    // hwbreak
    // multiprocess
    // vCont
    // error-message
    // NOTE: We don't have a limit, so we just set it to this.
    // Our PacketSize is 4096
    // NOTE: Despite supporting QStartNoAckMode, we will not send it as it will also tell gdb we prefer it, which we don't,
    // as serial connections aren't reliable (and neither is our driver).
    const char* const supported[] = {
        "swbreak",
        "hwbreak",
        "multiprocess",
        "vContSupported",
        "error-message",
    };
    const size_t packet_size = 4096;
    size_t responseLen = 0;
    char* response = nullptr;
    for (size_t i = 0; arguments[i]; )
    {
        size_t feature_len = strnchr(arguments + i, ';', argumentsLen-i)-1;
        if (arguments[i+feature_len-1] == '+')
            feature_len--;
        bool isSupported = false;
        size_t j = 0;
        for (; j < sizeof(supported)/sizeof(supported[0]) && !isSupported; j++)
            if (uacpi_strncmp(arguments + i, supported[j], feature_len) == 0)
                isSupported = true;
        if (isSupported)
        {
            // This bitfield was made specifically to match the indices of the supported array and have the same meaning.
            con->gdb_supported |= (1<<j);
            size_t off = responseLen;
            response = Kdbg_Realloc(response, (responseLen += (feature_len + 2)) + 1);
            memcpy(response + off, &arguments[i], feature_len);
            response[off + feature_len] = '+';
            response[off + feature_len+1] = ';';
        }
        if (arguments[i+feature_len] == '+')
            feature_len++;
        i += (feature_len + 1);
    }
    const char* format = "PacketSize=%lu;qXfer:exec-file:read+;binary-upload?;error-message+";
    size_t featureLen = snprintf(nullptr, 0, format, packet_size);
    response = Kdbg_Realloc(response, responseLen+featureLen+1);
    snprintf(response + responseLen, featureLen+1, format, packet_size);
    responseLen += featureLen;
    obos_status status = Kdbg_ConnectionSendPacket(con, response);
    Kdbg_Free(response);
    return status;
}
obos_status Kdbg_GDB_qAttached(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_USERDATA;
    NO_CTX;
    return Kdbg_ConnectionSendPacket(con, "1");
}
obos_status Kdbg_GDB_vMustReplyEmpty(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_USERDATA;
    NO_CTX;
    return Kdbg_ConnectionSendPacket(con, "");
}
static char* common_io_out(const char* arguments, size_t argumentsLen, size_t commandLen, uint16_t* out_ioaddr, uint32_t* out_iodata)
{
    if ((commandLen+1) >= argumentsLen)
        return "496E636F7272656374206E756D626572206F6620617267756D656E74730a"; // Incorrect number of arguments
    const char* endptr = nullptr;
    uint16_t io_addr = OBOSH_StrToULL(arguments+commandLen+1, &endptr, 0);
    if (!endptr)
        return "496E76616C696420617267756D656E740A"; // Invald argument
    printf("endptr=%s\n", endptr);
    endptr++;
    printf("endptr=%s\n", endptr);
    if (endptr >= (argumentsLen+arguments))
        return "496E76616C696420617267756D656E740A"; // Invald argument
    uint32_t data = OBOSH_StrToULL(endptr, &endptr, 0);
    if (!endptr)
        return "496E636F7272656374206E756D626572206F6620617267756D656E74730a"; // Incorrect number of arguments
    printf("io addr = 0x%04x, data = 0x%x\n", io_addr, data);
    *out_ioaddr = io_addr;
    *out_iodata = data;
    return "537563636573730A";
}
// void OBOS_TestLocks();
obos_status Kdbg_GDB_qRcmd(gdb_connection* con, const char* arguments_, size_t argumentsLen_, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_USERDATA;
    size_t argumentsLen = argumentsLen_ / 2;
    char* arguments = Kdbg_Calloc(argumentsLen + 1, sizeof(char));
    for (size_t i = 0; i < argumentsLen; i++)
        arguments[i] = KdbgH_hex2bin(arguments_ + (i*2), 2);
    // printf("%s\n%s\n", arguments, arguments_);
    size_t commandLen = strnchr(arguments, ' ', argumentsLen);
    char* response = "556E6B6E6F776E20636F6D6D616E640A";
    if (!commandLen)
        return Kdbg_ConnectionSendPacket(con, response);
    if (commandLen != argumentsLen)
        commandLen -= 1;
    char* command = 
        (char*)memcpy(Kdbg_Calloc(commandLen+1, sizeof(char)), arguments, commandLen);
    bool freeResponse = false;
    if (strcmp(command, "ping"))
        response = "706F6E670A";
    else if (strcmp(command, "io8_read"))
    {
        if ((commandLen+1) >= argumentsLen)
        {
            // Incorrect number of arguments
            response = "496E636F7272656374206E756D626572206F6620617267756D656E74730a";
            goto down;
        }
        const char* endptr = nullptr;
        uint16_t io_addr = OBOSH_StrToULL(arguments+commandLen+1, &endptr, 0);
        if (!endptr)
        {
            response = "496E76616C696420617267756D656E740A";
            goto down;
        }
        char* read = KdbgH_FormatResponse("%02x", inb(io_addr));
        response = KdbgH_FormatResponse("3078%02x%02x0a", read[0], read[1]);
        Kdbg_Free(read);
        freeResponse = true;
    }
    else if (strcmp(command, "io16_read"))
    {
        if ((commandLen+1) >= argumentsLen)
        {
            // Incorrect number of arguments
            response = "496E636F7272656374206E756D626572206F6620617267756D656E74730a";
            goto down;
        }
        const char* endptr = nullptr;
        uint16_t io_addr = OBOSH_StrToULL(arguments+commandLen+1, &endptr, 0);
        if (!endptr)
        {
            response = "496E76616C696420617267756D656E740A";
            goto down;
        }
        char* read = KdbgH_FormatResponse("%04x", inw(io_addr));
        response = KdbgH_FormatResponse("3078%02x%02x%02x%02x0a", read[0], read[1], read[2], read[3]);
        Kdbg_Free(read);
        freeResponse = true;
    }
    else if (strcmp(command, "io32_read"))
    {
        if ((commandLen+1) >= argumentsLen)
        {
            // Incorrect number of arguments
            response = "496E636F7272656374206E756D626572206F6620617267756D656E74730a";
            goto down;
        }
        const char* endptr = nullptr;
        uint16_t io_addr = OBOSH_StrToULL(arguments+commandLen+1, &endptr, 0);
        if (!endptr)
        {
            response = "496E76616C696420617267756D656E740A";
            goto down;
        }
        char* read = KdbgH_FormatResponse("%08x", ind(io_addr));
        response = KdbgH_FormatResponse("3078%02x%02x%02x%02x%02x%02x%02x%02x0a", read[0], read[1], read[2], read[3], read[4], read[5], read[6], read[7]);
        Kdbg_Free(read);
        freeResponse = true;
    }
    else if (strcmp(command, "io8_write"))
    {
        uint16_t io_addr = 0;
        uint32_t data = 0;
        response = common_io_out(arguments, argumentsLen, commandLen, &io_addr, &data);
        if (strcmp(response, "537563636573730A"))
        {
            printf("outb(0x%x, 0x%x)\n", io_addr, data & 0xff);
            outb(io_addr, data & 0xff);
        }
        freeResponse = false;
    }
    else if (strcmp(command, "io16_write"))
    {
        uint16_t io_addr = 0;
        uint32_t data = 0;
        response = common_io_out(arguments, argumentsLen, commandLen, &io_addr, &data);
        if (strcmp(response, "537563636573730A"))
        {
            printf("outw(0x%x, 0x%x)\n", io_addr, data & 0xffff);
            outw(io_addr, data & 0xffff);
        }
        freeResponse = false;
    }
    else if (strcmp(command, "io32_write"))
    {
        uint16_t io_addr = 0;
        uint32_t data = 0;
        response = common_io_out(arguments, argumentsLen, commandLen, &io_addr, &data);
        if (strcmp(response, "537563636573730A"))
        {
            printf("outd(0x%x, 0x%x)\n", io_addr, data & 0xff);
            outd(io_addr, data & 0xff);
        }
        freeResponse = false;
    }
    down:
    Kdbg_Free(arguments);
    Kdbg_Free(command);
    obos_status st = Kdbg_ConnectionSendPacket(con, response);
    if (freeResponse)
        Kdbg_Free(response);
    return st;
}

#define do_bounds_check(ptr, pstart, len) \
({\
    uintptr_t _start = (uintptr_t)pstart;\
    uintptr_t _p = (uintptr_t)ptr;\
    (_p >= _start) && (_p < (_start+len));\
})

obos_status Kdbg_GDB_qXfer(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_USERDATA;
    char* resp = "E.Unrecognized operation";
    bool free_resp = false;

    const char* op = arguments;
    size_t op_len = strnchr(arguments, ':', argumentsLen)-1;
    
    const char* op_type = nullptr;
    size_t len_op_type = 0;
    
    uint64_t annex = 0, offset = 0, length = 0;
    OBOS_MAYBE_UNUSED bool have_annex = false, have_offset = false, have_length = false;
    
    
    const char* op_args = op + (op_len+1);
    size_t op_args_len = argumentsLen - (op_len+1);
    
    const char* args_iter = op_args;
    size_t rel_args_size = op_args_len;
    do {
        const char* ptr = args_iter;
        size_t len_ptr = strnchr(op_args, ':', rel_args_size)-1;
        if (!do_bounds_check(ptr, op_args, op_args_len))
            return Kdbg_ConnectionSendPacket(con, "E.Not enough arguments");
        op_type = ptr;
        len_op_type = len_ptr;
        rel_args_size -= len_ptr + 1;
        args_iter += len_ptr + 1;
    } while(0);
    do {
        const char* ptr = args_iter;
        size_t len_ptr = strnchr(ptr, ':', rel_args_size)-1;
        if (!do_bounds_check(ptr, op_args, op_args_len))
            return Kdbg_ConnectionSendPacket(con, "E.Not enough arguments");
        annex = OBOSH_StrToULL(ptr, nullptr, 16);
        have_annex = len_ptr > 0;
        rel_args_size -= len_ptr + 1;
        args_iter += len_ptr + 1;
    } while(0);
    do {
        const char* ptr = args_iter;
        size_t len_ptr = strnchr(ptr, ',', rel_args_size)-1;
        if (!do_bounds_check(ptr, op_args, op_args_len))
            return Kdbg_ConnectionSendPacket(con, "E.Not enough arguments");
        offset = OBOSH_StrToULL(ptr, nullptr, 16);
        have_offset = len_ptr > 0;
        rel_args_size -= len_ptr + 1;
        args_iter += len_ptr + 1;
    } while(0);
    do {
        const char* ptr = args_iter;
        size_t len_ptr = rel_args_size;
        if (!do_bounds_check(ptr, op_args, op_args_len))
            return Kdbg_ConnectionSendPacket(con, "E.Not enough arguments");
        length = OBOSH_StrToULL(ptr, nullptr, 16);
        have_length = len_ptr > 0;
        rel_args_size -= len_ptr + 1;
        args_iter += len_ptr + 1;
    } while(0);

    if (uacpi_strncmp(op_type, "read", len_op_type) == 0)
    {
        const char* resp_buffer = nullptr;
        size_t len_resp_buffer = 0;
        bool resp_binary = false; 

        if (uacpi_strncmp(op, "exec-file", op_len) == 0)
        {
            process* proc = have_annex ? Core_LookupProc(annex-1) : dbg_ctx->interrupted_thread->proc;
            if (!proc)
                return Kdbg_ConnectionSendPacket(con, "E.Could not find process");
            resp_binary = false;
            if (proc->pid != 0)
                resp_buffer = proc->exec_file;
            else
                resp_buffer = "oboskrnl";

            len_resp_buffer = strlen(resp_buffer);
        }

        if (!resp_binary)
        {
            if (offset < len_resp_buffer)
                resp = KdbgH_FormatResponse("l%.*s", OBOS_MIN(len_resp_buffer-offset, length), resp_buffer + OBOS_MAX(offset, 0UL));
            else
                resp = KdbgH_FormatResponse("l");
        }
    }

    obos_status status = Kdbg_ConnectionSendPacket(con, resp);
    if (free_resp)
        Kdbg_Free(resp);
    return status;
}