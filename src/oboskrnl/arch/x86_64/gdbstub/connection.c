/*
 * oboskrnl/arch/x86_64/gdbstub/connection.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <stdarg.h>

#include <driver_interface/header.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/alloc.h>

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
    while(nRead < 1)
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
    size_t szRawPacket = 0;
    size_t capRawPacket = 0;
    const size_t step = 8;
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
        if (szRawPacket >= capRawPacket)
        {
            capRawPacket += step;
            rawPacket = Kdbg_Realloc(rawPacket, capRawPacket);
        }
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
            rawPacket[szRawPacket++] = ch;
        }
    }
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
        Kdbg_Free(rawPacket);
        capRawPacket = 0;
        szRawPacket = 0;
        goto retry;
    }
    if (szRawPacket >= capRawPacket)
    {
        capRawPacket += step;
        rawPacket = Kdbg_Realloc(rawPacket, capRawPacket);
    }
    rawPacket[szRawPacket] = 0;
    *packet = rawPacket;
    *szPacket_ = szRawPacket;
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