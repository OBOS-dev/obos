/*
 * oboskrnl/arch/x86_64/gdbstub/connection.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <cmdline.h>

#include <stdarg.h>

#include <driver_interface/header.h>

#include <arch/x86_64/idt.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/alloc.h>
#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/packet_dispatcher.h>
#include <arch/x86_64/gdbstub/debug.h>
#include <arch/x86_64/gdbstub/general_query.h>
#include <arch/x86_64/gdbstub/stop_reply.h>
#include <arch/x86_64/gdbstub/bp.h>

#include <utils/string.h>

#include <allocators/base.h>

enum
{
    FLAGS_ENABLE_ACK = 0x1,
};

static uint8_t mod256(const char* data, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++)
        checksum += data[i];
    return checksum;
}
uintptr_t KdbgH_hex2bin(const char* str, unsigned size)
{
    uintptr_t ret = 0;
    str += *str == '\n';
    for (int i = size - 1, j = 0; i > -1; i--, j++)
    {
        char c = str[i];
        uintptr_t digit = 0;
        switch (c)
        {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            digit = c - '0';
            break;
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
            digit = (c - 'A') + 10;
            break;
        case 'a':
        case 'b':
        case 'c':
        case 'd':
        case 'e':
        case 'f':
            digit = (c - 'a') + 10;
            break;
        default:
            break;
        }
        /*if (!j)
        {
            ret = digit;
            continue;
        }*/
        ret |= digit << (j * 4);
    }
    return ret;
}
// Must be a pipe-style driver, or stuff will go wrong.
obos_status Kdbg_ConnectionInitialize(gdb_connection* conn, const driver_ftable* pipe_interface, dev_desc pipe)
{
    if (!conn || !pipe_interface)
        return OBOS_STATUS_INVALID_ARGUMENT;
    conn->pipe_interface = pipe_interface;
    conn->pipe = pipe;
    conn->flags |= FLAGS_ENABLE_ACK;
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_ConnectionSendPacket(gdb_connection* conn, const char* packet)
{
    if (!conn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!conn->pipe_interface)
        return OBOS_STATUS_UNINITIALIZED;
    // printf(">%s\n", packet);
    uint8_t checksum = mod256(packet, strlen(packet));
    const char* format = "$%s#%02x";
    size_t szBuf = snprintf(nullptr, 0, format, packet, checksum);
    char* buf = Kdbg_Calloc(szBuf+1, sizeof(char));
    snprintf(buf, szBuf+1, format, packet, checksum);
    if (!(conn->flags & FLAGS_ENABLE_ACK))
    {
        return conn->pipe_interface->write_sync(
            conn->pipe,
            buf, szBuf,
            0,nullptr
        );
    }
    for (size_t i = 0; i < 5; i++)
    {
        obos_status status = conn->pipe_interface->write_sync(
            conn->pipe,
            buf, szBuf,
            0,nullptr
        );
        if (obos_is_error(status))
            return status;
        size_t nRead = 0;
        char ack = 0;
        size_t spin = 0;
        try_again:
        status = conn->pipe_interface->read_sync(
            conn->pipe,
            &ack, 1,
            0,&nRead
        );
        if ((spin++) >= 1000)
            return OBOS_STATUS_SUCCESS;
        if (nRead != 1)
            goto try_again;
        if (obos_is_error(status))
            return status;
        if (ack == '+')
            return OBOS_STATUS_SUCCESS;
        else if (ack == '-')
            continue;
        else
            return OBOS_STATUS_INTERNAL_ERROR; // something stupid has happened. the gdb stub's ack enabled value and gdb's ack enabled value are probably out of sync
    }
    return OBOS_STATUS_RETRY;
}
static char recv_char(gdb_connection* conn)
{
    size_t nRead = 0;
    const size_t nToRead = 1;
    char ret = 0;
    size_t spin = 0;
    while(nRead < nToRead && (spin++ < 100000))
        conn->pipe_interface->read_sync(conn->pipe, &ret, nToRead, 0, &nRead);
    return ret;
}
obos_status Kdbg_ConnectionRecvPacket(gdb_connection* conn, char** packet, size_t* szPacket_)
{
    if (!conn || !packet || !szPacket_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    retry:
    (void)0;
    char* rawPacket = nullptr;
    size_t rawPacketLen = 0;
    char ch = 0;
    uint8_t calculatedChecksum = 0;
    uint8_t remoteChecksum = 0;
    char checksum[2] = {};
    bool ack = true;
    size_t spin = 0;
    while((ch = recv_char(conn)) != '$' && spin++ < 500000)
        __builtin_ia32_pause();
    if (ch != '$')
    {
        ack = false;
        goto acknowledge;
    }
    bool isEscaped = false;
    while((ch = recv_char(conn)) != '#')
    {
        calculatedChecksum += ch;
        if (ch == '}' && !isEscaped)
            isEscaped = true;
        else
        {
            if (isEscaped)
            {
                isEscaped = false;
                ch ^= 0x20; // unescape the character.
            }
            size_t oldSize = rawPacketLen;
            rawPacketLen++;
            rawPacket = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, rawPacket, rawPacketLen, oldSize, nullptr);
            rawPacket[rawPacketLen - 1] = ch;
        }
    }
    size_t oldSize = rawPacketLen;
    rawPacket = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, rawPacket, rawPacketLen+1, oldSize, nullptr);
    rawPacket[rawPacketLen] = 0;
    checksum[0] = recv_char(conn);
    checksum[1] = recv_char(conn);
    remoteChecksum = KdbgH_hex2bin(checksum, 2);
    ack = remoteChecksum == calculatedChecksum;
    acknowledge:
    (void)0;
    char ackCh = ack ? '+' : '-';
    conn->pipe_interface->write_sync(conn->pipe, &ackCh, 1, 0, nullptr);
    if (!ack)
    {
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, rawPacket, rawPacketLen);
        goto retry;
    }
    *packet = rawPacket;
    *szPacket_ = rawPacketLen;
    return OBOS_STATUS_SUCCESS;
}
obos_status Kdbg_ConnectionSetAck(gdb_connection* conn, bool ack)
{
    if (!conn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (ack)
        conn->flags |= FLAGS_ENABLE_ACK;
    else
        conn->flags &= ~FLAGS_ENABLE_ACK;
    return OBOS_STATUS_SUCCESS;
}

char* KdbgH_FormatResponse(const char* format, ...)
{
    va_list list;
    va_list list2;
    va_start(list, format);
    va_copy(list2, list);
    size_t bufSize = vsnprintf(nullptr, 0, format, list);
    va_end(list);
    char* buf = Kdbg_Calloc(bufSize+1, sizeof(char));
    vsnprintf(buf, bufSize+1, format, list2);
    va_end(list2);
    return buf;
}
char* KdbgH_FormatResponseSized(size_t bufSize, const char* format, ...)
{
    va_list list;
    va_start(list, format);
    char* buf = Kdbg_Calloc(bufSize+1, sizeof(char));
    vsnprintf(buf, bufSize+1, format, list);
    va_end(list);
    return buf;
}

static bool initialized_handlers;
obos_status Kdbg_InitializeHandlers()
{
    if (initialized_handlers)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    Kdbg_AddPacketHandler("qC", Kdbg_GDB_qC, nullptr);
    Kdbg_AddPacketHandler("qfThreadInfo", Kdbg_GDB_q_ThreadInfo, nullptr);
    Kdbg_AddPacketHandler("qsThreadInfo", Kdbg_GDB_q_ThreadInfo, nullptr);
    Kdbg_AddPacketHandler("qAttached", Kdbg_GDB_qAttached, nullptr);
    Kdbg_AddPacketHandler("qSupported", Kdbg_GDB_qSupported, nullptr);
    Kdbg_AddPacketHandler("?", Kdbg_GDB_query_halt, nullptr);
    Kdbg_AddPacketHandler("g", Kdbg_GDB_g, nullptr);
    Kdbg_AddPacketHandler("G", Kdbg_GDB_G, nullptr);
    Kdbg_AddPacketHandler("k", Kdbg_GDB_k, nullptr);
    Kdbg_AddPacketHandler("vKill", Kdbg_GDB_k, nullptr);
    Kdbg_AddPacketHandler("H", Kdbg_GDB_H, nullptr);
    Kdbg_AddPacketHandler("T", Kdbg_GDB_T, nullptr);
    Kdbg_AddPacketHandler("qRcmd", Kdbg_GDB_qRcmd, nullptr);
    Kdbg_AddPacketHandler("m", Kdbg_GDB_m, nullptr);
    Kdbg_AddPacketHandler("M", Kdbg_GDB_M, nullptr);
    Kdbg_AddPacketHandler("c", Kdbg_GDB_c, nullptr);
    Kdbg_AddPacketHandler("C", Kdbg_GDB_C, nullptr);
    Kdbg_AddPacketHandler("s", Kdbg_GDB_s, nullptr);
    Kdbg_AddPacketHandler("Z0", Kdbg_GDB_Z0, nullptr);
    Kdbg_AddPacketHandler("z0", Kdbg_GDB_z0, nullptr);
    Kdbg_AddPacketHandler("D", Kdbg_GDB_D, nullptr);
    Arch_RawRegisterInterrupt(0x3, (uintptr_t)(void*)Kdbg_int3_handler);
    Arch_RawRegisterInterrupt(0x1, (uintptr_t)(void*)Kdbg_int1_handler);
    initialized_handlers = true;
    return OBOS_STATUS_SUCCESS;
}