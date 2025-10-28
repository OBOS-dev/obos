/*
 * oboskrnl/vfs/local_socket.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/socket.h>

extern socket_ops Vfs_LocalDgramSocketBackend;
extern socket_ops Vfs_LocalStreamSocketBackend;

