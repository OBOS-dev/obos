/*
 * oboskrnl/vfs/fd.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/vnode.h>
#include <vfs/fd.h>
#include <vfs/limits.h>
#include <vfs/dirent.h>
#include <vfs/pagecache.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>

#include <utils/list.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

obos_status Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags)
{
    if (!desc || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    dirent* ent = VfsH_DirentLookup(path);
    if (!ent)
        return OBOS_STATUS_NOT_A_FILE;
    OBOS_ASSERT(ent->vnode);
    desc->vn = ent->vnode;
    desc->flags |= FD_FLAGS_OPEN;
    desc->flags |= FD_FLAGS_READ;
    desc->flags |= FD_FLAGS_WRITE;
    if (desc->vn->owner_uid == Core_GetCurrentThread()->proc->currentUID)
    {
        // We have owner perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.owner_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.owner_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else if (desc->vn->group_uid == Core_GetCurrentThread()->proc->currentGID)
    {
        // We have group perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.group_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.group_write)
            desc->flags &= FD_FLAGS_READ;
    }
    else
    {
        // We have other perms.
        vnode* const vn = desc->vn;
        if (!vn->perm.other_read)
            desc->flags &= FD_FLAGS_READ;
        if (!vn->perm.other_write)
            desc->flags &= FD_FLAGS_READ;
    }
    if (oflags & FD_OFLAGS_READ_ONLY)
        desc->flags &= ~FD_FLAGS_WRITE;
    return OBOS_STATUS_SUCCESS;
}
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