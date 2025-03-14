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
    FD_FLAGS_NOEXEC = 16,
};
enum
{
    FD_OFLAGS_READ = 1,
    FD_OFLAGS_WRITE = 2,
    FD_OFLAGS_UNCACHED = 4,
    FD_OFLAGS_NOEXEC = 8,
    // NOTE: Only handled in syscalls, and is ignored in Vfs_FdOpen*.
    FD_OFLAGS_CREATE = 16,
};
typedef struct fd
{
    struct vnode* vn;
    uint32_t flags;
    uoff_t offset;
    LIST_NODE(fd_list, struct fd) node;
} fd;
OBOS_EXPORT obos_status       Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags);
OBOS_EXPORT obos_status Vfs_FdOpenDirent(fd* const desc, dirent* ent, uint32_t oflags);
OBOS_EXPORT obos_status  Vfs_FdOpenVnode(fd* const desc, void* vn, uint32_t oflags);
OBOS_EXPORT obos_status      Vfs_FdWrite(fd* desc, const void* buf, size_t nBytes, size_t* nWritten);
OBOS_EXPORT obos_status       Vfs_FdRead(fd* desc, void* buf, size_t nBytes, size_t* nRead);
OBOS_EXPORT obos_status     Vfs_FdAWrite(fd* desc, const void* buf, size_t nBytes, event* evnt);
OBOS_EXPORT obos_status      Vfs_FdARead(fd* desc, void* buf, size_t nBytes, event* evnt);
OBOS_EXPORT obos_status       Vfs_FdSeek(fd* desc, off_t off, whence_t whence);
OBOS_EXPORT uoff_t         Vfs_FdTellOff(const fd* desc);
OBOS_EXPORT size_t        Vfs_FdGetBlkSz(const fd* desc);
OBOS_EXPORT obos_status        Vfs_FdEOF(const fd* desc); 
OBOS_EXPORT struct vnode* Vfs_FdGetVnode(fd* desc);
OBOS_EXPORT obos_status      Vfs_FdIoctl(fd* desc, uint64_t request, void* argp);
OBOS_EXPORT obos_status      Vfs_FdFlush(fd* desc);
OBOS_EXPORT obos_status      Vfs_FdClose(fd* desc);
