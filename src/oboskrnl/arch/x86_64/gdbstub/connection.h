/*
 * oboskrnl/arch/x86_64/gdbstub/connection.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <driver_interface/header.h>

#include <arch/x86_64/gdbstub/breakpoint.h>

typedef struct gdb_connection
{
    // Communication context
    const driver_ftable* pipe_interface;
    dev_desc pipe;
    uint32_t flags;
    bool connection_active;

    // Connection context.
    struct
    {
        bool received_first;
        thread_node* last_thread;
    } q_ThreadInfo_ctx;
    // bool swbreak_supported;
    // bool hwbreak_supported;
    // bool multiprocess_supported;
    // bool vCont_supported;
    // bool errormessage_supported;
    // Bitfield:
    // bit 0: swbreak
    // bit 1: hwbreak
    // bit 2: multiprocess
    // bit 3: vCont
    // bit 4: error-message
    uint32_t gdb_supported;
    sw_breakpoint_list sw_breakpoints;
} gdb_connection;
typedef struct gdb_ctx
{
    thread* interrupted_thread;
    thread_ctx interrupt_ctx;
    bool wake;
} gdb_ctx;
extern OBOS_EXPORT gdb_connection* Kdbg_CurrentConnection;

uintptr_t KdbgH_hex2bin(const char* str, unsigned size);

// Must be a pipe-style driver, or stuff will go wrong.
obos_status Kdbg_ConnectionInitialize(gdb_connection* conn, const driver_ftable* pipe_interface, dev_desc pipe);
obos_status Kdbg_ConnectionSendPacket(gdb_connection* conn, const char* packet);
// *packet is guaranteed to be nul-terminated.
obos_status Kdbg_ConnectionRecvPacket(gdb_connection* conn, char** packet, size_t* szPacket);
// NOTE: Does not send the packet to change the ack status
// You must do that yourself.
obos_status Kdbg_ConnectionSetAck(gdb_connection* conn, bool ack); 

char* KdbgH_FormatResponse(const char* format, ...);
// size needs to be the exact size of the output, or bad things will happen.
char* KdbgH_FormatResponseSized(size_t size, const char* format, ...);