/*
 * oboskrnl/arch/x86_64/gdbstub/packet_dispatcher.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/x86_64/gdbstub/connection.h>

typedef obos_status(*packet_handler)(gdb_connection* con, const char* arguments, size_t argumentsLen, void* userdata);
void Kdbg_AddPacketHandler(const char* name, packet_handler handler, void* userdata);
// packet must be nul-terminated
// or bad stuff might happen.
obos_status Kdbg_DispatchPacket(gdb_connection* con, const char* packet, size_t packetLen);