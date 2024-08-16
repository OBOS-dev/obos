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
#include <vfs/alloc.h>
#include <vfs/fd.h>
#include <vfs/limits.h>
#include <vfs/dirent.h>
#include <vfs/pagecache.h>
#include <vfs/mount.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

// if depth == zero, it will search all nodes starting at 'from'
// otherwise, it will only search 'depth' nodes

static pagecache_ent* pagecache_lookup_from(vnode* vn, size_t off, pagecache_ent* from, size_t depth)
{
    if (!depth)
        depth = SIZE_MAX;
    Core_MutexAcquire(&vn->pagecache_lock);
    size_t i = 0;
    for (pagecache_ent* curr = from; curr && i < depth; i++)
    {
        if (off >= curr->fileoff && off < (curr->fileoff + curr->sz))
        {
            Core_MutexRelease(&vn->pagecache_lock);
            OBOS_Debug("Page cache hit!\n");
            return curr;
        }

        curr = LIST_GET_NEXT(pagecache, &vn->pagecache_entries, curr);
    }
    Core_MutexRelease(&vn->pagecache_lock);
    return nullptr;
}
static pagecache_ent* pagecache_lookup(vnode* vn, size_t off, size_t depth)
{
    return pagecache_lookup_from(vn, off, LIST_GET_HEAD(pagecache, &vn->pagecache_entries), depth);
}
static bool is_eof(vnode* vn, size_t off)
{
    return off >= vn->filesize;
}
obos_status Vfs_FdOpen(fd* const desc, const char* path, uint32_t oflags)
{
    if (!desc || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    dirent* ent = VfsH_DirentLookup(path);
    if (!ent)
        return OBOS_STATUS_NOT_FOUND;
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
    if (oflags & FD_OFLAGS_UNCACHED)
        desc->flags |= FD_FLAGS_UNCACHED;
    desc->vn->refs++;
    desc->flags |= FD_FLAGS_OPEN;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdWrite(fd* desc, const void* buf, size_t nBytes, size_t* nWritten);
static obos_status do_uncached_read(fd* desc, void* into, size_t nBytes, size_t* nRead_)
{
    const driver_header* const fs_drv = &desc->vn->mount_point->fs_driver->driver->header;
    obos_status status = fs_drv->ftable.read_sync(desc->vn->desc, into, nBytes, desc->offset, nRead_);
    if (obos_expect(obos_is_error(status) == true, 0))
        return status;
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_FdRead(fd* desc, void* buf, size_t nBytes, size_t* nRead)
{
    if (!desc || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (!nBytes)
        return OBOS_STATUS_SUCCESS;
    if (is_eof(desc->vn, desc->offset))
        return OBOS_STATUS_EOF;
    if (!(desc->flags & FD_FLAGS_READ))
        return OBOS_STATUS_ACCESS_DENIED;
    if (nBytes > (desc->vn->filesize - desc->offset))
        nBytes = desc->vn->filesize - desc->offset; // truncate size to the space we have left in the file.
    obos_status status = OBOS_STATUS_SUCCESS;
    if (desc->flags & FD_FLAGS_UNCACHED)
    {
        // Keep it nice and simple, and just do an uncached read on the file.

        status = do_uncached_read(desc, buf, nBytes, nRead);
    }
    else 
    {
        const driver_header* const fs_drv = &desc->vn->mount_point->fs_driver->driver->header;
        pagecache_ent* pc_ent;
        size_t read = 0;
        for (size_t i = desc->offset; !is_eof(desc->vn, i) && read < nBytes;)
        {
            pc_ent = pagecache_lookup(desc->vn, i, 0);
            try_again:
            if (!pc_ent)
            {
                // Make a page cache entry.
                pc_ent = (pagecache_ent*)Vfs_Calloc(1, sizeof(pagecache_ent));
                pc_ent->dirty = false;
                pc_ent->sz = nBytes-read;
                pc_ent->fileoff = i;
                pc_ent->data = Vfs_Calloc(nBytes-read, sizeof(char));
                status = fs_drv->ftable.read_sync(desc->vn->desc, pc_ent->data, nBytes-read, i, nullptr);
                if (obos_expect(obos_is_error(status), 0))
                    break;
                Core_MutexAcquire(&desc->vn->pagecache_lock);
                LIST_APPEND(pagecache, &desc->vn->pagecache_entries, pc_ent);
                Core_MutexRelease(&desc->vn->pagecache_lock);
                goto try_again;
            }
            size_t off = (i-pc_ent->fileoff);
            size_t nBytesLeft = pc_ent->sz-off;
            memcpy(buf + read, pc_ent->data + off, nBytesLeft);
            read += nBytesLeft;
            i += read;
        }
        if (nRead)
            *nRead = read;
    }
    if (obos_expect(obos_is_success(status), 1))
        desc->offset += nBytes;
    return status;
}
obos_status Vfs_FdSeek(fd* desc, off_t off, whence_t whence)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    size_t finalOff = 0;
    switch (whence)
    {
        case SEEK_SET:
            finalOff = off;
            break;
        case SEEK_END:
            finalOff = (off_t)desc->vn->filesize + off;
            break;
        case SEEK_CUR:
            finalOff = (off_t)desc->offset + off;
            break;
    }
    if (is_eof(desc->vn, finalOff))
        return OBOS_STATUS_EOF;
    desc->offset = finalOff;
    return OBOS_STATUS_SUCCESS;
}
uoff_t Vfs_FdTellOff(const fd* desc)
{
    if (desc)
        return desc->offset;
    return (uoff_t)(-1);
}
obos_status Vfs_FdEOF(const fd* desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    return is_eof(desc->vn, desc->offset) ? OBOS_STATUS_EOF : OBOS_STATUS_SUCCESS;
}
vnode* Vfs_FdGetVnode(fd* desc)
{
    return desc ? desc->vn : nullptr;
}
obos_status Vfs_FdIoctl(fd* desc, size_t nParameters, uint64_t request, ...)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!(desc->flags & FD_FLAGS_OPEN))
        return OBOS_STATUS_UNINITIALIZED;
    if (desc->vn->vtype != VNODE_TYPE_BLK && desc->vn->vtype != VNODE_TYPE_CHR)
        return OBOS_STATUS_INVALID_ARGUMENT;
    va_list list;
    va_start(list, request);
    obos_status status = desc->vn->un.device->driver->header.ftable.ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}
obos_status Vfs_FdFlush(fd* desc)
{
    OBOS_UNUSED(desc);
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status Vfs_FdClose(fd* desc)
{
    Vfs_FdFlush(desc);
    desc->flags &= ~FD_FLAGS_OPEN;
    vnode* vn = desc->vn;
    vn->refs--;
    return OBOS_STATUS_SUCCESS;
}
LIST_GENERATE(pagecache, pagecache_ent, node);