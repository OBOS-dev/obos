/*
 * oboskrnl/arch/x86_64/gdbstub/connection.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "uacpi/acpi.h"
#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

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
static uintptr_t hex2bin(const char* str, unsigned size)
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
        try_again:
        status = conn->pipe_interface->read_sync(
            conn->pipe,
            &ack, 1,
            0,&nRead
        );
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
obos_status Kdbg_ConnectionRecvPacket(gdb_connection* conn, char** packet, size_t* szPacket_)
{
    if (!conn || !packet || !szPacket_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    // Read the raw packet.
    retry:
    (void)0;
    char* raw_packet = nullptr;
    size_t offset = 0;
    size_t szPacket = 0;
    obos_status status = OBOS_STATUS_SUCCESS;
    bool foundChecksum = false;
    size_t szChecksum = 0;
    size_t checksumOffset = 0;
    bool isEscaped = false;
    bool foundEscapedChar = false;
    size_t nRetries = 0;
    if (nRetries == 5)
        return OBOS_STATUS_INTERNAL_ERROR;
    while (szChecksum < 2)
    {
        char ch = 0;
        size_t nRead = 0;
        status = conn->pipe_interface->read_sync(
            conn->pipe,
            &ch, 1,
            0,&nRead
        );
        if (nRead != 1)
            continue;
        if (obos_is_error(status))
            return status;
        if (!isEscaped)
            if (ch == '}')
                isEscaped = true;
            else {}
        else
        {
            foundEscapedChar = true;
            // ch ^= 0x20;
        }
        raw_packet = Kdbg_Realloc(raw_packet, ++szPacket);
        raw_packet[szPacket - 1] = ch;
        if (foundChecksum)
            szChecksum++;
        if (ch == '#' && !isEscaped)
        {
            foundChecksum = true;
            checksumOffset = szPacket;
        }
        if (foundEscapedChar && isEscaped)
        {
            foundEscapedChar = false;
            isEscaped = false;
        }
    }
    char ack = '+';
    while(offset < szPacket && raw_packet[0] != '$')
    {
        raw_packet++;
        offset++;
    }
    checksumOffset -= offset;
    if (offset == szPacket)
    {
        ack = '-';
        goto acknoledge;
    }
    uint8_t checksum = hex2bin(&raw_packet[checksumOffset], 2);
    size_t packetSz = checksumOffset-2;
    uint8_t calculatedChecksum = mod256(&raw_packet[1], packetSz);
    if (checksum != calculatedChecksum)
        ack = '-';
    acknoledge:
    status = conn->pipe_interface->write_sync(
        conn->pipe,
        &ack, 1,
        0,nullptr
    );
    if (obos_is_error(status))
        return status;
    if (ack == '-' && (conn->flags & FLAGS_ENABLE_ACK))
    {
        Kdbg_Free(raw_packet - offset);
        raw_packet = nullptr;
        packetSz = 0;
        goto retry;
    }
    *packet = Kdbg_Calloc(packetSz+1, sizeof(char));
    for (size_t i = 0; i < packetSz; i++)
    {
        bool canAdd = false;
        if (!isEscaped)
            if (raw_packet[i+1] == '}')
                isEscaped = true;
            else
                canAdd = true;
        else
        {
            raw_packet[i+1] ^= 0x20;
            canAdd = true;
        }
        if (canAdd)
        {
            (*packet)[i-isEscaped] = raw_packet[i+1];
            (*szPacket_)++;
        }
    }
    Kdbg_Free(raw_packet - offset);
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