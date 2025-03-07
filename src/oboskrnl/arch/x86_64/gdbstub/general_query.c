/*
 * oboskrnl/arch/x86_64/gdbstub/general_query.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>

#include <allocators/base.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/general_query.h>

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
    size_t featureLen = snprintf(nullptr, 0, "PacketSize=%lu", packet_size);
    response = Kdbg_Realloc(response, responseLen+featureLen+1);
    snprintf(response + responseLen, featureLen+1, "PacketSize=%lu", packet_size);
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
// void OBOS_TestLocks();
obos_status Kdbg_GDB_qRcmd(gdb_connection* con, const char* arguments_, size_t argumentsLen_, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_CTX;
    NO_USERDATA;
    size_t argumentsLen = argumentsLen_ / 2;
    char* arguments = Kdbg_Calloc(argumentsLen + 1, sizeof(char));
    for (size_t i = 0; i < argumentsLen; i++)
        arguments[i] = KdbgH_hex2bin(arguments_ + (i*2), 2);
    printf("%s\n%s\n", arguments, arguments_);
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
    Kdbg_Free(arguments);
    Kdbg_Free(command);
    obos_status st = Kdbg_ConnectionSendPacket(con, response);
    if (freeResponse)
        Kdbg_Free(response);
    return st;
}