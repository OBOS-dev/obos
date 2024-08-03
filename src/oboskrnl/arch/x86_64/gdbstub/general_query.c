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
    uint32_t tid = __builtin_bswap32(dbg_ctx->interrupted_thread->tid);
    uint32_t pid = __builtin_bswap32(dbg_ctx->interrupted_thread->proc->pid);
    char* response = KdbgH_FormatResponse("p%08x.%08x", pid, tid);
    Kdbg_ConnectionSendPacket(con, response);
    OBOS_Debug("Responding to qC with %s.\n", response);
    Kdbg_Free(response);
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_q_ThreadInfo(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_USERDATA;
    NO_CTX;
    uint32_t tids[4] = {};
    uint8_t nTids = 0;
    char* response = nullptr;
    bool shouldFreeResponse = true;
    if (!con->q_ThreadInfo_ctx.last_thread && con->q_ThreadInfo_ctx.received_first)
    {
        // Respond with an l.
        response = "l";
        shouldFreeResponse = false;
        con->q_ThreadInfo_ctx.received_first = false;
        goto respond;
    }
    for (con->q_ThreadInfo_ctx.received_first ? 0 : (con->q_ThreadInfo_ctx.last_thread = OBOS_KernelProcess->threads.head);
        con->q_ThreadInfo_ctx.last_thread && nTids < (sizeof(tids)/sizeof(*tids)); )
    {
        tids[nTids++] = __builtin_bswap32(con->q_ThreadInfo_ctx.last_thread->data->tid);

        con->q_ThreadInfo_ctx.last_thread = con->q_ThreadInfo_ctx.last_thread->next;
    }
    OBOS_ASSERT(nTids > 0);
    if (!con->q_ThreadInfo_ctx.received_first)
    {
        if (nTids == 1)
            response = KdbgH_FormatResponse("mp%08x.%08x", __builtin_bswap32(OBOS_KernelProcess->pid), tids[0]);
        else
        {
            // size is the 'm' + ppid.tid,ppid.tid,ppid.tid,ppid.tid\0
            response = Kdbg_Calloc(1+18*nTids+nTids, sizeof(char));
            for (uint8_t i = 0; i < nTids; i++)
            {
                char delimiter = ',';
                if (i == (nTids - 1))
                    delimiter = 0;
                char* tid = KdbgH_FormatResponse("p%08x.%08x%c", OBOS_KernelProcess->pid, tids[i], delimiter);
                memcpy(response + 1+18*i+i, tid, 18 + (delimiter == ','));
            }
            response[0] = 'm';
        }
    }
    else 
    {
        if (nTids == 1)
            response = KdbgH_FormatResponse("p%08x.%08x", OBOS_KernelProcess->pid, tids[0]);
        else
        {
            // size is ppid.tid,ppid.tid,ppid.tid,ppid.tid\0
            response = Kdbg_Calloc(18*nTids+nTids, sizeof(char));
            for (uint8_t i = 0; i < nTids; i++)
            {
                char* tid = KdbgH_FormatResponse("p%08x.%08x%c", OBOS_KernelProcess->pid, tids[i], i == (nTids - 1) ? '\0' : ',');
                memcpy(response + 1+18*i+((i > 0) ? 1 : 0), tid, 18 + (i == nTids-1));
            }
        }
    }
    OBOS_Debug("Responding to q_ThreadInfo with %s.\n", response);
    OBOS_ASSERT(response != nullptr);
    if (!con->q_ThreadInfo_ctx.received_first)
        con->q_ThreadInfo_ctx.received_first = true;
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
    for (size_t i = 0; i < argumentsLen; )
    {
        size_t feature_len = strchr(arguments + i, ';');
        bool isSupported = false;
        size_t j = 0;
        for (; j < sizeof(supported)/sizeof(supported[0]) && isSupported; j++)
            if (uacpi_strncmp(arguments + i, supported[j], feature_len))
                isSupported = true;
        if (isSupported)
        {
            size_t off = responseLen;
            response = Kdbg_Realloc(response, (responseLen += (feature_len + 1)) + 1);
            memcpy(response + off, &arguments[i], feature_len);
            response[off + feature_len-1] = '+';
            response[off + feature_len]   = '\0';
        }
        i += (feature_len + 1);
    }
    size_t featureLen = snprintf(nullptr, 0, "PacketSize=%lu", packet_size);
    response = Kdbg_Realloc(response, responseLen+featureLen);
    snprintf(response + responseLen, featureLen, "PacketSize=%lu", packet_size);
    responseLen += featureLen;
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_GDB_qAttached(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_USERDATA;
    NO_CTX;
    return Kdbg_ConnectionSendPacket(con, "1");
}
obos_status Kdbg_GDB_qRcmd(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata)
{
    NO_ARGUMENTS;
    NO_RESPONSE;
    NO_USERDATA;
    NO_CTX;
    return OBOS_STATUS_SUCCESS;
}