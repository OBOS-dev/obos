/*
 * oboskrnl/arch/x86_64/gdbstub/gdb_udp_backend.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/vnode.h>

#include <arch/x86_64/gdbstub/connection.h>

obos_status Kdbg_ConnectionInitializeUDP(gdb_connection* conn, uint16_t bind_port, vnode* interface);
