/*
 * oboskrnl/arch/x86_64/gdbstub/general_query.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/x86_64/gdbstub/connection.h>

obos_status Kdbg_GDB_qC(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_q_ThreadInfo(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_QStartNoAckMode(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_qSupported(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_qAttached(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_qRcmd(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);