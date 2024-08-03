/*
 * oboskrnl/arch/x86_64/gdbstub/connection.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>

typedef struct gdb_connection
{
    const driver_ftable* pipe_interface;
    dev_desc pipe;
    uint32_t flags;  
} gdb_connection;

// Must be a pipe-style driver, or stuff will go wrong.
obos_status Kdbg_ConnectionInitialize(gdb_connection* conn, const driver_ftable* pipe_interface, dev_desc pipe);
obos_status Kdbg_ConnectionSendPacket(gdb_connection* conn, const char* packet);
// *packet is guaranteed to be nul-terminated.
obos_status Kdbg_ConnectionRecvPacket(gdb_connection* conn, char** packet, size_t* szPacket);
// NOTE: Does not send the packet to change the ack status
// You must do that yourself.
obos_status Kdbg_ConnectionSetAck(gdb_connection* conn, bool ack); 