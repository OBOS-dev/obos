/*
 * oboskrnl/vfs/fd_sys.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <syscall.h>

#include <allocators/base.h>

#include <scheduler/cpu_local.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/swap.h>

#include <vfs/limits.h>
#include <vfs/fd.h>
#include <vfs/fd_sys.h>
#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

handle Sys_FdAlloc()
{
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
    desc->un.fd = Vfs_Calloc(1, sizeof(fd));
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    return ret;
}

obos_status Sys_FdOpen(handle desc, const char* upath, uint32_t oflags)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    if (strcmp(path, "/dev/stderr"))
    {
        Vfs_Free(fd->un.fd);
        fd->un.fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), 2, HANDLE_TYPE_FD, false, &status)->un.fd;
        return OBOS_STATUS_SUCCESS;
    }
    else if (strcmp(path, "/dev/stdout"))
    {
        Vfs_Free(fd->un.fd);
        fd->un.fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), 1, HANDLE_TYPE_FD, false, &status)->un.fd;
        return OBOS_STATUS_SUCCESS;
    }
    else if (strcmp(path, "/dev/stdin"))
    {
        Vfs_Free(fd->un.fd);
        fd->un.fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), 0, HANDLE_TYPE_FD, false, &status)->un.fd;
        return OBOS_STATUS_SUCCESS;
    }
    status = Vfs_FdOpen(fd->un.fd, path, oflags);
    Free(OBOS_KernelAllocator, path, sz_path);
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

obos_status Sys_FdOpenAt(handle desc, handle ent, const char* name, uint32_t oflags)
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

    dirent* real_dent = VfsH_DirentLookupFrom(name, dent->un.dirent);
    if (!real_dent)
        return OBOS_STATUS_NOT_FOUND;

    return Vfs_FdOpenDirent(fd->un.fd, real_dent, oflags);
}

obos_status Sys_FdWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    status = OBOS_STATUS_SUCCESS;
    void* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)buf, nullptr, nBytes, OBOS_PROTECTION_READ_ONLY, true, &status);
    if (obos_is_error(status))
        return status;

    size_t nWritten_ = 0;
    status = Vfs_FdWrite(fd->un.fd, kbuf, nBytes, &nWritten_);
    if (nWritten)
        memcpy_k_to_usr(nWritten, &nWritten_, sizeof(size_t));

    // if (desc == 1 || desc == 2)
    //     printf("%.*s", nBytes, kbuf);

    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);
        return status;
    }

    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    status = OBOS_STATUS_SUCCESS;
    void* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)buf, nullptr, nBytes, 0, true, &status);
    if (obos_is_error(status))
        return status;

    size_t nRead_ = 0;
    status = Vfs_FdRead(fd->un.fd, kbuf, nBytes, &nRead_);

    if (nRead)
        memcpy_k_to_usr(nRead, &nRead_, sizeof(size_t));

    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);
        return status;
    }

    return OBOS_STATUS_SUCCESS;
}

// I/O System might go through a rewrite soon...
// Leave these unimplemented until then.

obos_status Sys_FdAWrite(handle desc, const void* buf, size_t nBytes, handle evnt)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(buf);
    OBOS_UNUSED(nBytes);
    OBOS_UNUSED(evnt);
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status Sys_FdARead(handle desc, void* buf, size_t nBytes, handle evnt)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(buf);
    OBOS_UNUSED(nBytes);
    OBOS_UNUSED(evnt);
    return OBOS_STATUS_UNIMPLEMENTED;
}

obos_status Sys_FdSeek(handle desc, off_t off, whence_t whence)
{
    // for (volatile bool b = (desc == 0x1); b;)
    //     OBOSS_SpinlockHint();

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Vfs_FdSeek(fd->un.fd, off, whence);
}

uoff_t Sys_FdTellOff(const handle desc)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return (uoff_t)-1;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    OBOS_ENSURE(fd->un.fd);
    return Vfs_FdTellOff(fd->un.fd);
}

size_t Sys_FdGetBlkSz(const handle desc)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Vfs_FdGetBlkSz(fd->un.fd);
}

obos_status Sys_FdEOF(const handle desc)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Vfs_FdEOF(fd->un.fd);
}

obos_status Sys_FdIoctl(handle desc, uint64_t request, void* argp, size_t sz_argp)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    void* kargp = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, argp, nullptr, sz_argp, 0, true, &status);
    if (obos_is_error(status))
        return status;

    status = Vfs_FdIoctl(fd->un.fd, request, kargp);
    Mm_VirtualMemoryFree(&Mm_KernelContext, kargp, sz_argp);

    return status;
}

obos_status Sys_FdFlush(handle desc)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Vfs_FdFlush(fd->un.fd);
}

// socket
#define S_IFSOCK   0140000
// symbolic link
#define S_IFLNK    0120000
// regular file
#define S_IFREG    0100000
// block device
#define S_IFBLK    0060000
// directory
#define S_IFDIR    0040000
// character device
#define S_IFCHR    0020000
// FIFO
#define S_IFIFO    0010000

obos_status Sys_Stat(int fsfdt, handle desc, const char* upath, int flags, struct stat* target)
{
    if (!target || !fsfdt)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct stat st = {};
    obos_status status = memcpy_k_to_usr(target, &st, sizeof(st));
    if (obos_is_error(status))
        return status;
    struct vnode* to_stat = nullptr;
    switch (fsfdt) {
        case FSFDT_FD:
        {
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            status = OBOS_STATUS_SUCCESS;
            handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
            if (!fd)
            {
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                return status;
            }
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            to_stat = fd->un.fd->vn;
            break;
        }
        case FSFDT_PATH:
            // path is relative to the CWD (TODO)
            // just fallthrough until that gets implemented
        case FSFDT_FD_PATH:
        {
            char* path = nullptr;
            size_t sz_path = 0;
            status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
            if (obos_is_error(status))
                return status;
            path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
            OBOSH_ReadUserString(upath, path, nullptr);
            dirent* dent = VfsH_DirentLookup(path);
            Free(OBOS_KernelAllocator, path, sz_path);
            if (dent)
                to_stat = dent->vnode;
            else
                status = OBOS_STATUS_NOT_FOUND;
            break;
        }
        default: return OBOS_STATUS_INVALID_ARGUMENT;
    }
    if (!to_stat)
        return status;
    st.st_size = to_stat->filesize;
    mount* const point = to_stat->mount_point ? to_stat->mount_point : to_stat->un.mounted;
    const driver_header* driver = (to_stat->vtype == VNODE_TYPE_REG || to_stat->vtype == VNODE_TYPE_DIR) ? &point->fs_driver->driver->header : nullptr;
    if (to_stat->vtype == VNODE_TYPE_CHR || to_stat->vtype == VNODE_TYPE_BLK)
        driver = &to_stat->un.device->driver->header;
    size_t blkSize = 0;
    size_t blocks = 0;
    OBOS_ENSURE(driver);
    OBOS_ENSURE(to_stat);
    driver->ftable.get_blk_size(to_stat->desc, &blkSize);
    driver->ftable.get_max_blk_count(to_stat->desc, &blocks);
    st.st_blksize = blkSize;
    st.st_mode = *(uint16_t*)&to_stat->perm;
    switch (to_stat->vtype) {
        case VNODE_TYPE_DIR:
            st.st_mode |= S_IFDIR;
            break;
        case VNODE_TYPE_FIFO:
            st.st_mode |= S_IFIFO;
            break;
        case VNODE_TYPE_CHR:
            st.st_mode |= S_IFCHR;
            break;
        case VNODE_TYPE_BLK:
            st.st_mode |= S_IFBLK;
            break;
        case VNODE_TYPE_REG:
            st.st_mode |= S_IFREG;
            break;
        case VNODE_TYPE_SOCK:
            st.st_mode |= S_IFSOCK;
            break;
        case VNODE_TYPE_LNK:
            st.st_mode |= S_IFLNK;
            break;
        default:
            OBOS_ENSURE(!"unimplemented");
    }
    st.st_size = to_stat->filesize;
    if (to_stat->vtype != VNODE_TYPE_CHR && to_stat->vtype != VNODE_TYPE_BLK)
    {
        drv_fs_info fs_info = {};
        OBOS_ENSURE (to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info);
        to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info(to_stat->mount_point->device, &fs_info);
        st.st_blocks = (to_stat->filesize+(fs_info.fsBlockSize-(to_stat->filesize%fs_info.fsBlockSize)))/512;
    }
    st.st_gid = to_stat->group_uid;
    st.st_uid = to_stat->owner_uid;
    // TODO: Inode numbers.
    st.st_ino = 0;
    memcpy_k_to_usr(target, &st, sizeof(struct stat));
    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_StatFSInfo(handle desc, drv_fs_info* info)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    if (!info)
        return OBOS_STATUS_INVALID_ARGUMENT;

    drv_fs_info out = {};
    status = memcpy_k_to_usr(info, &out, sizeof(out));
    if (obos_is_error(status))
        return status;

    vnode* vn = dent->un.dirent->vnode;
    status = Vfs_StatFSInfo(vn, &out);
    if (obos_is_error(status))
        return status;

    return OBOS_STATUS_SUCCESS;
}

handle Sys_OpenDir(const char* upath, obos_status *statusp)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
    {
        if (statusp)
            memcpy_k_to_usr(statusp, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    dirent* dent = VfsH_DirentLookup(path);
    Free(OBOS_KernelAllocator, path, sz_path);
    if (!dent)
    {
        status = OBOS_STATUS_NOT_FOUND;
        if (statusp)
            memcpy_k_to_usr(statusp, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }

    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_DIRENT, &desc);
    desc->un.dirent = dent->d_children.head;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    if (statusp)
        memcpy_k_to_usr(statusp, &status, sizeof(obos_status));

    return ret;
}

obos_status Sys_ReadEntries(handle desc, void* buffer, size_t szBuf, size_t* nRead)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    size_t k_nRead = 0;

    if (dent->un.dirent == (dirent*)-1)
    {
        if (nRead)
            memcpy_k_to_usr(nRead, &k_nRead, sizeof(size_t));
        return OBOS_STATUS_SUCCESS;
    }

    void* kbuff = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer, nullptr, szBuf, 0, true, &status);
    if (!kbuff)
        return status;

    memzero(kbuff, szBuf);

    status = Vfs_ReadEntries(dent->un.dirent, kbuff, szBuf, &dent->un.dirent, nRead ? &k_nRead : nullptr);
    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuff, szBuf);
    if (!dent->un.dirent)
        dent->un.dirent = (dirent*)-1;
    if (obos_is_error(status))
        return status;

    if (nRead)
        memcpy_k_to_usr(nRead, &k_nRead, sizeof(size_t));

    return status;
}

static handle alloc_fd(handle_table* tbl)
{
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(tbl);
    handle ret = OBOS_HandleAllocate(tbl, HANDLE_TYPE_FD, &desc);
    desc->un.fd = Vfs_Calloc(1, sizeof(fd));
    OBOS_UnlockHandleTable(tbl);
    return ret;
}

void OBOS_OpenStandardFDs(handle_table* tbl)
{
    handle hnd_stdin = alloc_fd(tbl);
    handle hnd_stdout = alloc_fd(tbl);
    handle hnd_stderr = alloc_fd(tbl);
    OBOS_LockHandleTable(tbl);
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* stdin = OBOS_HandleLookup(tbl, hnd_stdin, HANDLE_TYPE_FD, false, &status);
    handle_desc* stdout = OBOS_HandleLookup(tbl, hnd_stdout, HANDLE_TYPE_FD, false, &status);
    handle_desc* stderr = OBOS_HandleLookup(tbl, hnd_stderr, HANDLE_TYPE_FD, false, &status);
    OBOS_UnlockHandleTable(tbl);
    Vfs_FdOpen(stdin->un.fd, "/dev/COM1", FD_OFLAGS_READ);
    Vfs_FdOpen(stdout->un.fd, "/dev/COM1", FD_OFLAGS_WRITE);
    Vfs_FdOpen(stderr->un.fd, "/dev/COM1", FD_OFLAGS_WRITE);
}

// Writebacks all dirty pages in the page cache back to disk.
// Do this twice, in case a file gets flushed, and then the filesystem driver
// makes new dirty pages.
void Sys_Sync()
{
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);
}