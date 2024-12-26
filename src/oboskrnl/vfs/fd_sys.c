/*
 * oboskrnl/vfs/fd_sys.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include "allocators/base.h"
#include <int.h>
#include <error.h>
#include <handle.h>
#include <syscall.h>

#include <vfs/limits.h>
#include <vfs/fd.h>
#include <vfs/alloc.h>

handle Sys_FdAlloc()
{
    handle_desc* desc = nullptr;
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
    desc->un.fd = Vfs_Calloc(1, sizeof(fd));
    return ret;
}

obos_status Sys_FdOpen(handle desc, const char* upath, uint32_t oflags)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
        return status;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, sz_path, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    status = Vfs_FdOpen(fd->un.fd, path, oflags);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, path, sz_path);
    return status;
}

obos_status Sys_FdOpenDirent(handle desc, handle ent, uint32_t oflags)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }

    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ent, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Vfs_FdOpenDirent(fd->un.fd, dent->un.dirent, oflags);
}

obos_status Sys_FdWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten)
{
    return OBOS_STATUS_UNIMPLEMENTED;
}

obos_status       Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead) { return OBOS_STATUS_UNIMPLEMENTED; }

obos_status     Sys_FdAWrite(handle desc, const void* buf, size_t nBytes, handle evnt) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status      Sys_FdARead(handle desc, void* buf, size_t nBytes, handle evnt) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status       Sys_FdSeek(handle desc, off_t off, whence_t whence) { return OBOS_STATUS_UNIMPLEMENTED; }
uoff_t         Sys_FdTellOff(const handle desc) { return OBOS_STATUS_UNIMPLEMENTED; }
size_t        Sys_FdGetBlkSz(const handle desc) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status        Sys_FdEOF(const handle desc) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status      Sys_FdIoctl(handle desc, uint64_t request, void* argp, size_t sz_argp) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status      Sys_FdFlush(handle desc) { return OBOS_STATUS_UNIMPLEMENTED; }
obos_status      Sys_FdClose(handle desc) { return OBOS_STATUS_UNIMPLEMENTED; }
