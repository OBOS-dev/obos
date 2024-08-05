/*
 * oboskrnl/arch/x86_64/gdbstub/bp.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/x86_64/gdbstub/connection.h>

// Removes a software breakpoint.
obos_status Kdbg_GDB_z0(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata);
// Adds a software breakpoint.
obos_status Kdbg_GDB_Z0(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* dbg_ctx, void* userdata);