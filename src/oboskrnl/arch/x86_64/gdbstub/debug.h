/*
 * oboskrnl/arch/x86_64/gdbstub/debug.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/x86_64/interrupt_frame.h>

#include <arch/x86_64/gdbstub/connection.h>

#include <irq/dpc.h>

void Kdbg_int3_handler(interrupt_frame* frame);
void Kdbg_int1_handler(interrupt_frame* frame);

void Kdbg_GeneralDebugExceptionHandler(dpc*, gdb_connection* conn, gdb_ctx* dbg_ctx);