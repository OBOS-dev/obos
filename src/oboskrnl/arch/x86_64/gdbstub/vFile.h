/*
 * oboskrnl/arch/x86_64/gdbstub/vFile.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <handle.h>
#include <error.h>

#include <arch/x86_64/gdbstub/connection.h>

extern handle_table Kdbg_GDBHandleTable;

obos_status Kdbg_GDB_vFile(gdb_connection* con, const char* arguments, size_t argumentsLen, gdb_ctx* ctx, void* userdata);