/*
 * oboskrnl/arch/x86_64/gdbstub/stop_reply.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/x86_64/gdbstub/connection.h>

// '?' packet
// Queries the stop reason.
obos_status Kdbg_GDB_query_halt(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Read Registers
obos_status Kdbg_GDB_g(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Write Registers
obos_status Kdbg_GDB_G(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Kill the kernel (shutdown)
obos_status Kdbg_GDB_k(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Detach from gdb.
obos_status Kdbg_GDB_D(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Read Memory
obos_status Kdbg_GDB_m(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Write Memory
obos_status Kdbg_GDB_M(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Queries thread status.
obos_status Kdbg_GDB_T(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Step
obos_status Kdbg_GDB_s(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
// Continue [at address]
obos_status Kdbg_GDB_c(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);
obos_status Kdbg_GDB_C(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata);
// These packets: g,G,k,m,M
// But multithreaded
obos_status Kdbg_GDB_H(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);