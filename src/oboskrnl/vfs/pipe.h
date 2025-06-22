/*
 * oboskrnl/vfs/pipe.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <locks/event.h>
#include <locks/pushlock.h>

// fds is an array of 2 file descriptors
obos_status Vfs_CreatePipe(fd* fds, size_t pipesize);
obos_status Vfs_CreateNamedPipe(file_perm perm, gid group_uid, uid owner_uid, const char* parentpath, const char* name, size_t pipesize);

// Called in Vfs_Initialize.
void Vfs_InitializePipeInterface();

// !O_NONBLOCK, n <= PIPE_BUF: Atomic writes, block if no room
//  O_NONBLOCK, n <= PIPE_BUF: Atomic writes, return OBOS_STATUS_TRY_AGAIN if no room.
// !O_NONBLOCK,  n > PIPE_BUF: Unatomic writes, blocks until data is written (which includes blocking until the pipe is full).
//  O_NONBLOCK,  n > PIPE_BUF: Unatomic writes, return OBOS_STATUS_TRY_AGAIN if no room. Note: Partial writes are possible (check nWritten).
#define PIPE_BUF (512 /* minimum specified by POSIX */)

typedef struct pipe_desc
{
    event evnt;
    vnode *vn;
    void* buf;
    size_t pipe_size;
    _Atomic(uintptr_t) offset;
    pushlock lock;
} pipe_desc;
