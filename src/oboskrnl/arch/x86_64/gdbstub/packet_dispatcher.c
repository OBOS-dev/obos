/*
 * oboskrnl/arch/x86_64/gdbstub/packet_dispatcher.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <arch/x86_64/gdbstub/connection.h>
#include <arch/x86_64/gdbstub/packet_dispatcher.h>
#include <arch/x86_64/gdbstub/alloc.h>

#include <utils/tree.h>

#include <uacpi_libc.h>

typedef struct gdb_packet
{
    char* packet;
    size_t strlen_packet;
    packet_handler handler;
    void* userdata;
    RB_ENTRY(gdb_packet) node;
} gdb_packet;
RB_HEAD(gdb_packet_tree, gdb_packet);
static struct gdb_packet_tree packet_handlers;

static int cmp_packet(const gdb_packet *lhs, const gdb_packet *rhs)
{
    if (lhs->strlen_packet < rhs->strlen_packet)
        return -1;
    if (lhs->strlen_packet > rhs->strlen_packet)
        return 1;
    return uacpi_memcmp(lhs->packet, rhs->packet, lhs->strlen_packet);
}

RB_PROTOTYPE_STATIC(gdb_packet_tree, gdb_packet, node, cmp_packet);

void Kdbg_AddPacketHandler(const char* name, packet_handler handler, void* userdata)
{
    if (!name || !handler)
        return;
    size_t nameLen = strlen(name);
    gdb_packet *packet = Kdbg_Calloc(1, sizeof(gdb_packet));
    *packet = (gdb_packet){
        .packet = memcpy(Kdbg_Calloc(nameLen + 1, sizeof(char)), name, nameLen),
        .strlen_packet = nameLen,
        .handler = handler,
        .userdata = userdata
    };
    RB_INSERT(gdb_packet_tree, &packet_handlers, packet);
}
obos_status Kdbg_DispatchPacket(gdb_connection* con, const char* packet, size_t packetLen, gdb_ctx* ctx)
{
    if (!con || !packet || !packetLen)
        return OBOS_STATUS_INVALID_ARGUMENT;
    const char* name = nullptr;
    size_t nameLen = 1;
    size_t argsOffset = 0;
    switch (*packet) {
        case 'v':
        {
            nameLen = strnchr(packet, ';', packetLen) == packetLen ? 
                strnchr(packet, '?', packetLen) : 
                strnchr(packet, ';', packetLen);
            if (nameLen != packetLen)
                nameLen--;
            name = packet;
            break;
        }
        case 'Q':
        case 'q':
        {
            size_t comma_offset = strnchr(packet, ',', packetLen);
            size_t colon_offset = strnchr(packet, ':', packetLen);
            nameLen = colon_offset == packetLen ? ((comma_offset == packetLen) ? packetLen : comma_offset-1) : colon_offset-1;
            name = packet;
            argsOffset = nameLen + (nameLen!=packetLen);
            break;
        }
        case 'z':
        case 'Z':
        {
            nameLen = 2;
            name = packet;
            argsOffset = nameLen+(nameLen!=packetLen);
            break;
        }
        default:
        {
            argsOffset = 1;
            name = packet;
            nameLen = 1;
            break;
        }
    }
    const char* arguments = packet + argsOffset;
    size_t szArguments = packetLen - argsOffset;
	gdb_packet key = {
        .packet = (char*)name,
        .strlen_packet = nameLen,
    };
    const gdb_packet *found_packet = RB_FIND(gdb_packet_tree, &packet_handlers, &key);
    if (!found_packet)
    {
        Kdbg_ConnectionSendPacket(con, "");
        return OBOS_STATUS_UNHANDLED;
    }
    if (name != packet)
        Kdbg_Free((char*)name);
    return found_packet->handler(con, arguments, szArguments, ctx, found_packet->userdata);
}

RB_GENERATE_STATIC(gdb_packet_tree, gdb_packet, node, cmp_packet);
