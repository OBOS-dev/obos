/*
 * oboskrnl/vfs/pipe.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <locks/event.h>
#include <locks/pushlock.h>
#include <locks/mutex.h>

// fds is an array of 2 file descriptors
obos_status Vfs_CreatePipe(fd* fds, size_t pipesize);
obos_status Vfs_CreateNamedPipe(file_perm perm, gid group_uid, uid owner_uid, dirent* parent, const char* name, size_t pipesize);

// Called in Vfs_Initialize.
void Vfs_InitializePipeInterface();

// !O_NONBLOCK, n <= PIPE_BUF: Atomic writes, block if no room
//  O_NONBLOCK, n <= PIPE_BUF: Atomic writes, return OBOS_STATUS_TRY_AGAIN if no room.
// !O_NONBLOCK,  n > PIPE_BUF: Unatomic writes, blocks until data is written (which includes blocking until the pipe is full).
//  O_NONBLOCK,  n > PIPE_BUF: Unatomic writes, return OBOS_STATUS_TRY_AGAIN if no room. Note: Partial writes are possible (check nWritten).
#define PIPE_BUF (512 /* minimum specified by POSIX */)

typedef struct pipe_desc
{
    vnode* vn;
    size_t size;
    void* buf;
    intptr_t in_ptr;
    intptr_t ptr;
    const char* ptr_last_mod;
    const char* in_ptr_last_mod;
    event data_evnt;
    event empty_evnt;
    event write_evnt;
    // locks access to buffer and size
    // read_sync and write_sync don't modify
    // those variables, so they can take this
    // as a reader.
    pushlock buffer_lock;
    mutex ptr_lock;
    size_t refs;
} pipe_desc;
