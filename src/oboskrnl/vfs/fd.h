/*
 * oboskrnl/vfs/fd.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/vnode.h>
#include <vfs/limits.h>

#include <utils/list.h>

typedef LIST_HEAD(fd_list, struct fd) fd_list;
LIST_PROTOTYPE(fd_list, struct fd, node);
enum
{
    FD_FLAGS_OPEN,
    FD_FLAGS_READ,
    FD_FLAGS_WRITE,
};
enum
{
    FD_OFLAGS_READ_ONLY,
    FD_OFLAGS_TRUNC,
};
typedef struct fd
{
    vnode* vn;
    uint32_t flags;
    uoff_t offset;
    LIST_NODE(fd_list, struct fd) node;
} fd;
obos_status  Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags);
obos_status Vfs_FdWrite(fd desc, const void* buf, size_t nBytes, size_t* nWritten);
obos_status  Vfs_FdRead(fd desc, void* buf, size_t nBytes, size_t* nRead);
obos_status  Vfs_FdSeek(fd desc, off_t off, whence_t whence);
uoff_t    Vfs_FdTellOff(fd desc);
// Returns OBOS_STATUS_EOF on EOF, OBOS_STATUS_SUCCESS if not on EOF. anything else is an error.
obos_status   Vfs_FdEOF(fd desc); 
vnode*   Vfs_FdGetVnode(fd desc);
obos_status Vfs_FdIoctl(fd desc, size_t nParameters, uint64_t request, ...);
obos_status Vfs_FdFlush(fd desc);
obos_status Vfs_FdClose(fd desc);