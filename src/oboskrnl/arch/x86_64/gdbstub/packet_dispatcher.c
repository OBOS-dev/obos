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

#include <utils/hashmap.h>

#include <uacpi_libc.h>

typedef struct gdb_packet
{
    char* packet;
    size_t strlen_packet;
    packet_handler handler;
    void* userdata;
} gdb_packet;
static struct hashmap* packet_handlers = nullptr;
static uint64_t hash_packet(const void *item, uint64_t seed0, uint64_t seed1)
{
    const struct gdb_packet* pck = item;
    return hashmap_sip(pck->packet, pck->strlen_packet, seed0, seed1);
}
static int cmp_packet(const void *a, const void *b, void *udata)
{
    OBOS_UNUSED(udata);
    const struct gdb_packet* pck1 = a;
    const struct gdb_packet* pck2 = b;
    return uacpi_strcmp(pck1->packet, pck2->packet);
}
static void initialize_hashmap()
{
    packet_handlers = hashmap_new_with_allocator(
        Kdbg_Malloc, Kdbg_Realloc, Kdbg_Free,
        sizeof(gdb_packet), 
        64, 0, 0, 
        hash_packet, cmp_packet, 
        Kdbg_Free, 
        nullptr);
}
void Kdbg_AddPacketHandler(const char* name, packet_handler handler, void* userdata)
{
    if (!name || !handler)
        return;
    if (!packet_handlers)
        initialize_hashmap();
    size_t nameLen = strlen(name);
    gdb_packet packet = {
        .packet = memcpy(Kdbg_Calloc(nameLen + 1, sizeof(char)), name, nameLen),
        .strlen_packet = nameLen,
        .handler = handler,
        .userdata = userdata
    };
    hashmap_set(packet_handlers, &packet);
}
obos_status Kdbg_DispatchPacket(gdb_connection* con, const char* packet, size_t packetLen, gdb_ctx* ctx)
{
    if (!packet_handlers)
        initialize_hashmap();
    if (!con || !packet || !packetLen)
        return OBOS_STATUS_INVALID_ARGUMENT;
    char* name = nullptr;
    size_t nameLen = 1;
    size_t argsOffset = 0;
    switch (*packet) {
        case 'v':
        {
            nameLen = strchr(packet, ';') == packetLen ? strchr(packet, '?') : strchr(packet, ';');
            if (nameLen != packetLen)
                nameLen--;
            name = Kdbg_Calloc(nameLen + 1, sizeof(char));
            memcpy(name, packet, nameLen);
            break;
        }
        case 'Q':
        case 'q':
        {
            size_t comma_offset = strchr(packet, ',');
            size_t colon_offset = strchr(packet, ':');
            nameLen = colon_offset == packetLen ? ((comma_offset == packetLen) ? packetLen : comma_offset-1) : colon_offset-1;
            name = Kdbg_Calloc(nameLen + 1, sizeof(char));
            memcpy(name, packet, nameLen);
            argsOffset = nameLen + (nameLen!=packetLen);
            break;
        }
        case 'z':
        case 'Z':
        {
            nameLen = 2;
            name = Kdbg_Calloc(nameLen + 1, sizeof(char));
            memcpy(name, packet, nameLen);
            argsOffset = nameLen+(nameLen!=packetLen);
            break;
        }
        default:
        {
            argsOffset = 1;
            name = Kdbg_Calloc(nameLen + 1, sizeof(char));
            name[0] = *packet;
            break;
        }
    }
    const char* arguments = packet + argsOffset;
    size_t szArguments = packetLen - argsOffset;
	const gdb_packet key = {
        .packet = name,
        .strlen_packet = nameLen,
    };
    const gdb_packet *found_packet = hashmap_get(packet_handlers, &key);
    if (!found_packet)
    {
        Kdbg_ConnectionSendPacket(con, "");
        return OBOS_STATUS_UNHANDLED;
    }
    if (!strcmp(found_packet->packet, name))
    {
        Kdbg_ConnectionSendPacket(con, "");
        return OBOS_STATUS_UNHANDLED;
    }
    Kdbg_Free(name);
    return found_packet->handler(con, arguments, szArguments, ctx, found_packet->userdata);
}