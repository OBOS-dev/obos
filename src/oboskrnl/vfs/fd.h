/*
 * oboskrnl/vfs/fd.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/dirent.h>
#include <vfs/limits.h>

#include <utils/list.h>

#include <locks/event.h>

typedef LIST_HEAD(fd_list, struct fd) fd_list;
LIST_PROTOTYPE(fd_list, struct fd, node);
enum
{
    FD_FLAGS_OPEN = 1,
    FD_FLAGS_READ = 2,
    FD_FLAGS_WRITE = 4,
    FD_FLAGS_UNCACHED = 8,
};
enum
{
    FD_OFLAGS_READ_ONLY = 1,
    FD_OFLAGS_UNCACHED = 2,
};
typedef struct fd
{
    struct vnode* vn;
    uint32_t flags;
    uoff_t offset;
    LIST_NODE(fd_list, struct fd) node;
} fd;
obos_status       Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags);
obos_status       Vfs_FdOpenDirent(fd* const desc, dirent* ent, uint32_t oflags);
obos_status      Vfs_FdWrite(fd* desc, const void* buf, size_t nBytes, size_t* nWritten);
obos_status       Vfs_FdRead(fd* desc, void* buf, size_t nBytes, size_t* nRead);
obos_status     Vfs_FdAWrite(fd* desc, const void* buf, size_t nBytes, event* evnt);
obos_status      Vfs_FdARead(fd* desc, void* buf, size_t nBytes, event* evnt);
obos_status       Vfs_FdSeek(fd* desc, off_t off, whence_t whence);
uoff_t         Vfs_FdTellOff(const fd* desc);
size_t        Vfs_FdGetBlkSz(const fd* desc);
obos_status        Vfs_FdEOF(const fd* desc); 
struct vnode* Vfs_FdGetVnode(fd* desc);
obos_status      Vfs_FdIoctl(fd* desc, size_t nParameters, uint64_t request, ...);
obos_status      Vfs_FdFlush(fd* desc);
obos_status      Vfs_FdClose(fd* desc);