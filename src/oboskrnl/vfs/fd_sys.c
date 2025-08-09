/*
 * oboskrnl/vfs/fd_sys.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <signal.h>
#include <klog.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <syscall.h>
#include <partition.h>

#include <allocators/base.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/swap.h>

#include <utils/list.h>

#include <vfs/pipe.h>
#include <vfs/limits.h>
#include <vfs/fd.h>
#include <vfs/fd_sys.h>
#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>
#include <vfs/irp.h>
#include <vfs/create.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <irq/timer.h>

#include <driver_interface/driverId.h>

handle Sys_FdAlloc()
{
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
    desc->un.fd = Vfs_Calloc(1, sizeof(fd));
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    return ret;
}

static size_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return SIZE_MAX;
}

static file_perm unix_to_obos_mode(uint32_t mode)
{
    file_perm real_mode;
    if (mode & 001)
        real_mode.other_exec = true;
    if (mode & 002)
        real_mode.other_write = true;
    if (mode & 004)
        real_mode.other_read = true;
    if (mode & 010)
        real_mode.group_exec = true;
    if (mode & 020)
        real_mode.group_write = true;
    if (mode & 040)
        real_mode.group_read = true;
    if (mode & 100)
        real_mode.owner_exec = true;
    if (mode & 200)
        real_mode.owner_write = true;
    if (mode & 400)
        real_mode.owner_read = true;
    return real_mode;
}
obos_status Sys_FdOpenEx(handle desc, const char* upath, uint32_t oflags, uint32_t mode)
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
    status = Vfs_FdOpen(fd->un.fd, path, oflags & ~FD_OFLAGS_CREATE);
    if (status == OBOS_STATUS_NOT_FOUND && (oflags & FD_OFLAGS_CREATE))
    {
        size_t index = strrfind(path, '/');
        bool index_bumped = false;
        if (index == 0)
        {
            index_bumped = true;
            index++;
        }
        if (index == SIZE_MAX)
            index = 0;
        char ch = path[index];
        path[index] = 0;
        
        const char* dirname = path;
        dirent* parent = VfsH_DirentLookup(dirname);
        path[index] = ch;
        if (!parent)
        {
            Free(OBOS_KernelAllocator, path, sz_path);
            return OBOS_STATUS_NOT_FOUND; // parent wasn't found.
        }
        file_perm real_mode = unix_to_obos_mode(mode);
        status = Vfs_CreateNode(parent, path+(index == 0 ? 0 : (index_bumped ? index : index+1)), VNODE_TYPE_REG, real_mode);
        if (obos_is_error(status))
            goto err;
        dirent* ent = VfsH_DirentLookupFrom(path+(index == 0 ? 0 : (index_bumped ? index : index+1)), parent);
        if (index_bumped)
            index--;
        OBOS_ENSURE(ent);
        status = Vfs_FdOpenDirent(fd->un.fd, ent, oflags);
    }
    err:
//  printf("opened %s on fd 0x%x\n", path, desc);
    Free(OBOS_KernelAllocator, path, sz_path);
    return status;
}
obos_status Sys_FdOpen(handle desc, const char* upath, uint32_t oflags)
{
    return Sys_FdOpenEx(desc, upath, oflags & ~FD_OFLAGS_CREATE, 0);
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

    return Vfs_FdOpenDirent(fd->un.fd, dent->un.dirent, oflags & ~FD_OFLAGS_CREATE);
}

#define AT_FDCWD (handle)-100

obos_status Sys_FdOpenAt(handle desc, handle ent, const char* name, uint32_t oflags)
{
    return Sys_FdOpenAtEx(desc, ent, name, oflags & ~FD_OFLAGS_CREATE, 0);
}
obos_status Sys_FdOpenAtEx(handle desc, handle ent, const char* uname, uint32_t oflags, uint32_t mode)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }

    dirent* real_dent = nullptr;
    dirent* parent_dent = nullptr;
    if (ent == AT_FDCWD)
        parent_dent = Core_GetCurrentThread()->proc->cwd;
    else 
    {
        handle_desc* dent = nullptr;
        dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ent, HANDLE_TYPE_DIRENT, false, &status);
        if (!dent)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        parent_dent = dent->un.dirent;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    char* name = nullptr;
    size_t sz_name = 0;
    status = OBOSH_ReadUserString(uname, nullptr, &sz_name);
    if (obos_is_error(status))
        return status;
    name = ZeroAllocate(OBOS_KernelAllocator, sz_name+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uname, name, nullptr);
    real_dent = VfsH_DirentLookupFrom(name, parent_dent);

    if (!real_dent)
    {
        if (~oflags & FD_OFLAGS_CREATE)
            return OBOS_STATUS_NOT_FOUND;
        status = Vfs_CreateNode(parent_dent, name, VNODE_TYPE_REG, unix_to_obos_mode(mode));
        if (obos_is_error(status))
        {
            Free(OBOS_KernelAllocator, name, sz_name);
            return status;
        }
        real_dent = VfsH_DirentLookupFrom(name, parent_dent);
        OBOS_ASSERT(real_dent);
    }
    
    Free(OBOS_KernelAllocator, name, sz_name);
    return Vfs_FdOpenDirent(fd->un.fd, real_dent, oflags & ~FD_OFLAGS_CREATE);
}
obos_status Sys_FdCreat(handle desc, const char* name, uint32_t mode)
{
    return Sys_FdOpenEx(desc, name, FD_OFLAGS_CREATE|FD_OFLAGS_WRITE, mode);
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

    if (fd->un.fd->vn->seals & F_SEAL_WRITE)
        return OBOS_STATUS_ACCESS_DENIED;
    if ((fd->un.fd->vn->seals & F_SEAL_GROW) && (fd->un.fd->offset + nBytes) > fd->un.fd->vn->filesize)
        return OBOS_STATUS_ACCESS_DENIED;

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

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);

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

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_FdPWrite(handle desc, const void* buf, size_t nBytes, size_t* nWritten, size_t offset)
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
    status = Vfs_FdPWrite(fd->un.fd, kbuf, offset, nBytes, &nWritten_);
    if (nWritten)
        memcpy_k_to_usr(nWritten, &nWritten_, sizeof(size_t));

    // if (desc == 1 || desc == 2)
    //     printf("%.*s", nBytes, kbuf);

    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);
        return status;
    }

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);

    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_FdPRead(handle desc, void* buf, size_t nBytes, size_t* nRead, size_t offset)
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
    status = Vfs_FdPRead(fd->un.fd, kbuf, offset, nBytes, &nRead_);

    if (nRead)
        memcpy_k_to_usr(nRead, &nRead_, sizeof(size_t));

    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);
        return status;
    }

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, nBytes);

    return OBOS_STATUS_SUCCESS;
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

    if (sz_argp == SIZE_MAX)
    {
        if (fd->un.fd->vn->un.device->driver->header.ftable.ioctl_argp_size)
            status = fd->un.fd->vn->un.device->driver->header.ftable.ioctl_argp_size(request, &sz_argp);
        else
            status = OBOS_STATUS_UNIMPLEMENTED;
        if (obos_is_error(status))
            return status;
    }

    void* kargp = (sz_argp != 0) ? Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, argp, nullptr, sz_argp, 0, true, &status) : 0;
    if (obos_is_error(status))
        return status;

    status = Vfs_FdIoctl(fd->un.fd, request, kargp);
    if (kargp)
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

#define AT_SYMLINK_NOFOLLOW 0x100

obos_status Sys_Stat(int fsfdt, handle desc, const char* upath, int flags, struct stat* target)
{
    OBOS_UNUSED(flags && "Unimplemented");
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
//          printf("trying stat of %s\n", path);
            Free(OBOS_KernelAllocator, path, sz_path);
            if (dent && (~flags & AT_SYMLINK_NOFOLLOW && dent->vnode->vtype == VNODE_TYPE_LNK))
                dent = VfsH_FollowLink(dent);
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
    st.st_mode = 0;
    if (to_stat->perm.owner_read)
        st.st_mode |= 0400;
    if (to_stat->perm.owner_write)
        st.st_mode |= 0200;
    if (to_stat->perm.owner_exec)
        st.st_mode |= 0100;
    if (to_stat->perm.group_read)
        st.st_mode |= 040;
    if (to_stat->perm.group_write)
        st.st_mode |= 020;
    if (to_stat->perm.group_exec)
        st.st_mode |= 010;
    if (to_stat->perm.other_read)
        st.st_mode |= 004;
    if (to_stat->perm.other_write)
        st.st_mode |= 002;
    if (to_stat->perm.other_exec)
        st.st_mode |= 001;
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
    if (to_stat->vtype != VNODE_TYPE_CHR && to_stat->vtype != VNODE_TYPE_BLK && to_stat->vtype != VNODE_TYPE_FIFO && (~to_stat->flags & VFLAGS_EVENT_DEV))
    {
        drv_fs_info fs_info = {};
        OBOS_ENSURE (to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info);
        to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info(to_stat->mount_point->device, &fs_info);
        st.st_blocks = (to_stat->filesize+(fs_info.fsBlockSize-(to_stat->filesize%fs_info.fsBlockSize)))/512;
        st.st_blksize = fs_info.fsBlockSize;
    }
    st.st_gid = to_stat->group_uid;
    st.st_uid = to_stat->owner_uid;
    st.st_ino = to_stat->inode;   
    if (to_stat->flags & VFLAGS_EVENT_DEV)
        goto done;
    mount* const point = to_stat->mount_point ? to_stat->mount_point : to_stat->un.mounted;
    const driver_header* driver = (to_stat->vtype == VNODE_TYPE_REG || to_stat->vtype == VNODE_TYPE_DIR || to_stat->vtype == VNODE_TYPE_LNK) ? &point->fs_driver->driver->header : nullptr;
    if (to_stat->vtype == VNODE_TYPE_CHR || to_stat->vtype == VNODE_TYPE_BLK || to_stat->vtype == VNODE_TYPE_FIFO)
        driver = &to_stat->un.device->driver->header;
    size_t blkSize = 0;
    size_t blocks = 0;
    OBOS_ENSURE(driver);
    OBOS_ENSURE(to_stat);
    driver->ftable.get_blk_size(to_stat->desc, &blkSize);
    driver->ftable.get_max_blk_count(to_stat->desc, &blocks);
    st.st_blksize = blkSize;
    done:
    memcpy_k_to_usr(target, &st, sizeof(struct stat));
    return OBOS_STATUS_SUCCESS;
}

#define AT_REMOVEDIR 0x200

obos_status Sys_UnlinkAt(handle parent, const char* upath, int flags)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    dirent* node = nullptr;
    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    if (parent == AT_FDCWD || path[0] == '/')
    {
        dirent* dent = VfsH_DirentLookup(path);
        Free(OBOS_KernelAllocator, path, sz_path);
        if (!dent)
            return OBOS_STATUS_NOT_FOUND;

        node = dent;
    }
    else if (parent != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* parent_dirent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), parent, HANDLE_TYPE_DIRENT, false, &status);
        if (!parent_dirent)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());        

        dirent* dent = VfsH_DirentLookupFrom(path, parent_dirent->un.dirent);
        Free(OBOS_KernelAllocator, path, sz_path);
        if (!dent)
            return OBOS_STATUS_NOT_FOUND;

        node = dent;
    }

    if (node->vnode->vtype == VNODE_TYPE_DIR && ~flags & AT_REMOVEDIR)
        return OBOS_STATUS_NOT_A_FILE;

    return Vfs_UnlinkNode(node);
}

obos_status Sys_ReadLinkAt(handle parent, const char *upath, void* ubuff, size_t max_size, size_t* length)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    vnode* vn = nullptr;
    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);
    if (parent == AT_FDCWD || path[0] == '/')
    {
        dirent* dent = VfsH_DirentLookup(path);
        Free(OBOS_KernelAllocator, path, sz_path);
        if (!dent)
            return OBOS_STATUS_NOT_FOUND;

        vn = dent->vnode;
    }
    else if (!strlen(path))
    {
        Free(OBOS_KernelAllocator, path, sz_path);
        if (HANDLE_TYPE(parent) != HANDLE_TYPE_FD && HANDLE_TYPE(parent) != HANDLE_TYPE_DIRENT)
            return OBOS_STATUS_INVALID_ARGUMENT;
        
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), parent, HANDLE_TYPE_FD, true, &status);
        if (!fd)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        switch (HANDLE_TYPE(parent)) {
            case HANDLE_TYPE_FD:
                vn = fd->un.fd->vn;
                break;
            case HANDLE_TYPE_DIRENT:
                vn = fd->un.dirent->vnode;
                break;
            default:
                OBOS_UNREACHABLE;
        }   
    }

    if (!vn)
        return OBOS_STATUS_NOT_FOUND;
    
    if (vn->vtype != VNODE_TYPE_LNK)
        return OBOS_STATUS_INVALID_ARGUMENT;

    size_t len = OBOS_MIN(max_size, strlen(vn->un.linked));
    void* buff = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, ubuff, nullptr, max_size, 0, true, nullptr);
    memcpy(buff, vn->un.linked, OBOS_MIN(max_size, len));
    if (length)
        memcpy_k_to_usr(length, &len, sizeof(size_t));

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

    dent = VfsH_FollowLink(dent);

    Vfs_PopulateDirectory(dent);
    
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

obos_status Sys_Mkdir(const char* upath, uint32_t mode)
{
    char* path = nullptr;
    size_t sz_path = 0;
    obos_status status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);

    dirent* found = VfsH_DirentLookup(path);
    if (found)
        return OBOS_STATUS_ALREADY_INITIALIZED;

    size_t index = strrfind(path, '/');
    if (index == (sz_path-1))
    {
        path[index] = 0;
        index = strrfind(path, '/');
    }
    if (index == SIZE_MAX)
        index = 0;
    char ch = path[index];
    path[index] = 0;
    const char* dirname = path;
    // printf("dirname = %s\n", dirname);
    dirent* parent = VfsH_DirentLookup(dirname);
    path[index] = ch;
    if (!parent)
    {
        Free(OBOS_KernelAllocator, path, sz_path);
        return OBOS_STATUS_NOT_FOUND; // parent wasn't found.
    }
    // printf("%s\n", OBOS_GetStringCPtr(&parent->name));
    file_perm real_mode = unix_to_obos_mode(mode);
    // printf("%s\n", path);
    status = Vfs_CreateNode(parent, path+(!index ? index : index+1), VNODE_TYPE_DIR, real_mode);
    Free(OBOS_KernelAllocator, path, sz_path);
    // printf("%d\n", status);
    return status;
}
obos_status Sys_MkdirAt(handle ent, const char* uname, uint32_t mode)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ent, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }

    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    char* name = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(uname, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    name = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uname, name, nullptr);

    status = Vfs_CreateNode(dent->un.dirent, name, VNODE_TYPE_DIR, unix_to_obos_mode(mode));
    Free(OBOS_KernelAllocator, name, sz_path);

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
    Vfs_FdOpenVnode(stdin->un.fd, Core_GetCurrentThread()->proc->controlling_tty->vn, FD_OFLAGS_READ);
    Vfs_FdOpenVnode(stdout->un.fd, Core_GetCurrentThread()->proc->controlling_tty->vn, FD_OFLAGS_WRITE);
    Vfs_FdOpenVnode(stderr->un.fd, Core_GetCurrentThread()->proc->controlling_tty->vn, FD_OFLAGS_WRITE);
}

// Writebacks all dirty pages in the page cache back to disk.
// Do this twice, in case a file gets flushed, and then the filesystem driver
// makes new dirty pages.
void Sys_Sync()
{
    Mm_PageWriterOperation = PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);
}

static driver_id* detect_fs_driver(vnode* vn)
{
    if (vn->nPartitions == 1)
        return vn->partitions[0].fs_driver;
    else
        return nullptr;
} 

obos_status Sys_Mount(const char* uat, const char* uon)
{
    if (!uat || !uon)
        return OBOS_STATUS_INVALID_ARGUMENT;

    char* at = nullptr;
    size_t sz_path = 0;
    obos_status status = OBOSH_ReadUserString(uat, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    at = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uat, at, nullptr);

    char* on = nullptr;
    size_t sz_path_2 = 0;
    status = OBOSH_ReadUserString(uon, nullptr, &sz_path_2);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, at, sz_path);
        return status;
    }
    on = ZeroAllocate(OBOS_KernelAllocator, sz_path_2 + 1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uon, on, nullptr);

    dirent* ent = VfsH_DirentLookup(on);
    if (!ent)
    {
        status = OBOS_STATUS_NOT_FOUND;
        goto done;
    }

    vdev dev = { .driver=detect_fs_driver(ent->vnode) };
    if (!dev.driver)
        status = OBOS_STATUS_INVALID_ARGUMENT;
    else
        status = Vfs_Mount(at, ent->vnode, &dev, nullptr);

    done:

    Free(OBOS_KernelAllocator, at, sz_path);
    Free(OBOS_KernelAllocator, on, sz_path_2);

    return status;
}

obos_status Sys_Unmount(const char* uat)
{
    char* at = nullptr;
    size_t sz_path = 0;
    obos_status status = OBOSH_ReadUserString(uat, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    at = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uat, at, nullptr);

    status = Vfs_UnmountP(at);

    Free(OBOS_KernelAllocator, at, sz_path);

    return status;
}

handle Sys_IRPCreate(handle file, size_t offset, size_t size, bool dry, enum irp_op operation, void* buffer, obos_status* ustatus)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    switch (operation) {
        case IRP_READ:
        case IRP_WRITE:
            break;
        default:
            status = OBOS_STATUS_INVALID_ARGUMENT;
            if (ustatus) memcpy_k_to_usr(ustatus, &status, sizeof(obos_status));
            return HANDLE_INVALID;
    }

    vnode* vn = nullptr;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), file, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        if (ustatus) memcpy_k_to_usr(ustatus, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }
    vn = fd->un.fd->vn;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    if (!vn)
        return OBOS_STATUS_UNINITIALIZED;

    void* buff = dry ? nullptr : Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer, nullptr, size, operation == IRP_READ ? 0 : OBOS_PROTECTION_READ_ONLY, true, &status);

    user_irp* obj = nullptr;
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
    desc->un.irp = obj = Vfs_Calloc(1, sizeof(user_irp));
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    obj->obj = VfsH_IRPAllocate();

    VfsH_IRPBytesToBlockCount(vn, size, &obj->obj->blkCount);
    VfsH_IRPBytesToBlockCount(vn, offset, &obj->obj->blkOffset);
    obj->obj->op = operation;
    obj->obj->dryOp = !!dry;
    obj->obj->buff = buff;
    obj->obj->vn = vn;
    obj->obj->status = OBOS_STATUS_SUCCESS;
    
    return ret;
}
obos_status Sys_IRPSubmit(handle desc)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return VfsH_IRPSubmit(irph->un.irp->obj, nullptr);
}

obos_status Sys_IRPWait(handle desc, obos_status* out_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */, bool close)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    
    if (!out_status && !nCompleted)
        return OBOS_STATUS_INVALID_ARGUMENT;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    
    irp* req = irph->un.irp->obj;
    status = VfsH_IRPWait(req);
    if (out_status)
        memcpy_k_to_usr(out_status, &status, sizeof(status));
    if (nCompleted)
        memcpy_k_to_usr(nCompleted, &req->nBlkRead, sizeof(*nCompleted));

    if (close)
        return Sys_HandleClose(desc);

    return OBOS_STATUS_SUCCESS;
}

// Returns OBOS_STATUS_WOULD_BLOCK if the IRP has not completed, otherwise OBOS_STATUS_SUCCESS, or an error code.
obos_status Sys_IRPQueryState(handle desc)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    
    irp* req = irph->un.irp->obj;
    status = Core_EventGetState(req->evnt) ? OBOS_STATUS_SUCCESS : OBOS_STATUS_WOULD_BLOCK;
    return status;
}

obos_status Sys_IRPGetBuffer(handle desc, void** ubuffp)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    if (!ubuffp)
        return OBOS_STATUS_INVALID_ARGUMENT;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    
    return memcpy_k_to_usr(ubuffp, &irph->un.irp->ubuffer, sizeof(void*));
}
obos_status Sys_IRPGetStatus(handle desc, obos_status* out_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    
    if (!out_status && !nCompleted)
        return OBOS_STATUS_INVALID_ARGUMENT;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    
    irp* req = irph->un.irp->obj;
    status = req->status;
    if (!Core_EventGetState(req->evnt))
        status = OBOS_STATUS_IRP_RETRY;
    if (out_status)
        memcpy_k_to_usr(out_status, &status, sizeof(status));
    if (nCompleted)
        memcpy_k_to_usr(nCompleted, &req->nBlkRead, sizeof(*nCompleted));

    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_CreatePipe(handle* ufds, size_t pipesize)
{
    fd* kfds = Vfs_Calloc(2, sizeof(fd));
    obos_status status = Vfs_CreatePipe(kfds, pipesize);
    if (obos_is_error(status))
    {
        Vfs_Free(kfds);
        return status;
    }
    handle tmp[2] = {0,0};
    handle_desc *tmp_descs[2] = {nullptr,nullptr};
    tmp[0] = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &tmp_descs[0]);
    tmp[1] = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &tmp_descs[1]);
    tmp_descs[0] = OBOS_CurrentHandleTable()->arr+tmp[0];
    tmp_descs[0]->un.fd = Vfs_Malloc(sizeof(fd));
    tmp_descs[1]->un.fd = Vfs_Malloc(sizeof(fd));
    memcpy(tmp_descs[0]->un.fd, &kfds[0], sizeof(fd));
    memcpy(tmp_descs[1]->un.fd, &kfds[1], sizeof(fd));
    Vfs_FdClose(&kfds[0]);
    Vfs_FdClose(&kfds[1]);
    memzero(&tmp_descs[0]->un.fd->node, sizeof(tmp_descs[0]->un.fd->node));
    memzero(&tmp_descs[1]->un.fd->node, sizeof(tmp_descs[1]->un.fd->node));
    LIST_APPEND(fd_list, &tmp_descs[0]->un.fd->vn->opened, tmp_descs[0]->un.fd);
    LIST_APPEND(fd_list, &tmp_descs[1]->un.fd->vn->opened, tmp_descs[1]->un.fd);
    tmp_descs[0]->un.fd->vn->refs++;
    tmp_descs[1]->un.fd->vn->refs++;
    Vfs_Free(kfds);

    return memcpy_k_to_usr(ufds, tmp, sizeof(handle)*2);
}
obos_status Sys_CreateNamedPipe(handle dirfd, const char* upath, int mode, size_t pipesize)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    char* path = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, path, sz_path);
        return status;
    }
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);

    file_perm perm = unix_to_obos_mode(mode);
    const char* fifo_name = nullptr;

    dirent* parent = nullptr;
    if (dirfd != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            Free(OBOS_KernelAllocator, path, sz_path);
            return status;
        }
        parent = desc->un.dirent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

        if (strchr(path, '/') != strlen(path))
        {
            size_t last_slash = strrfind(path, '/');
            char ch = path[last_slash];
            path[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(path, parent);
            path[last_slash] = ch;
            fifo_name = path+last_slash+1;
        }
        else
            fifo_name = path;
    }
    else 
    {
        parent = Core_GetCurrentThread()->proc->cwd;
        if (strchr(path, '/') != strlen(path))
        {
            size_t last_slash = strrfind(path, '/');
            char ch = path[last_slash];
            path[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(path, parent);
            path[last_slash] = ch;
            fifo_name = path+last_slash+1;
        }
        else
            fifo_name = path;
    }

    gid current_gid = Core_GetCurrentThread()->proc->currentGID;
    uid current_uid = Core_GetCurrentThread()->proc->currentUID;
    status = Vfs_CreateNamedPipe(perm, current_gid, current_uid, parent, fifo_name, pipesize);
    Free(OBOS_KernelAllocator, path, sz_path);
    
    return status;
}

#define set_foreach(set, size) \
for (size_t _i = 0; _i < size; _i++)\
    for (size_t _j = 0, fd = _i * 8; _j < 8; _j++, fd++) \
        if ((set)[_i] & BIT(_j))

bool fd_avaliable_for(enum irp_op op, handle ufd, obos_status *status, irp** oreq)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ufd, HANDLE_TYPE_FD, false, status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return false;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    irp* req = VfsH_IRPAllocate();
    req->dryOp = true;
    req->op = op;
    req->vn = fd->un.fd->vn;
    req->blkCount = 1;
    VfsH_IRPBytesToBlockCount(req->vn, fd->un.fd->offset, &req->blkOffset);
    *status = VfsH_IRPSubmit(req, nullptr);
    if (obos_is_error(*status))
    {
        VfsH_IRPUnref(req);
        return false;
    }
    bool res = !req->evnt;
    if (req->evnt && req->evnt->signaled)
        res = true;
    if (res)
        VfsH_IRPUnref(req);
    else
        *oreq = req;
    return res;
}

void pselect_tm_handler(void* udata)
{
    event* evnt = udata;
    Core_EventSet(evnt, true);
}

obos_status Sys_PSelect(size_t nFds, uint8_t* uread_set, uint8_t *uwrite_set, uint8_t *uexcept_set, const struct pselect_extra_args* uextra)
{
    struct pselect_extra_args extra = {};
    obos_status status = memcpy_usr_to_k(&extra, uextra, sizeof(extra));
    if (obos_is_error(status))
        return status;
    if (nFds > 1024)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!uread_set && !uwrite_set && !uexcept_set)
        return OBOS_STATUS_SUCCESS; // We waited for nothing, so assume success.
    OBOS_UNUSED(uexcept_set && "We can't really monitor exceptional cases...");
    
    uint8_t *read_set = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, uread_set, nullptr, 128, 0, true, &status);
    if (obos_is_error(status) && uread_set)
        return status;
    uint8_t* write_set = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, uwrite_set, nullptr, 128, 0, true, &status);
    if (obos_is_error(status) && uwrite_set)
    {
        Mm_VirtualMemoryFree(CoreS_GetCPULocalPtr()->currentContext, read_set, 128);
        return status;
    }


    sigset_t sigmask = 0, oldmask = 0;
    if (extra.sigmask)
    {
        status = memcpy_usr_to_k(&sigmask, extra.sigmask, sizeof(sigset_t));
        if (obos_is_error(status))
            goto out2;
        OBOS_SigProcMask(SIG_SETMASK, &sigmask, &oldmask);
    }

    uint8_t read_set_tmp[128];
    uint8_t write_set_tmp[128];
    memzero(read_set_tmp, 128);
    memzero(write_set_tmp, 128);
    int num_events = 0;
    size_t nPossibleEvents = 0;
    if (read_set)
        set_foreach(read_set, 128)
            nPossibleEvents++;
    if (write_set)
        set_foreach(write_set, 128)
            nPossibleEvents++;
    irp** unsignaledIRPs = ZeroAllocate(OBOS_NonPagedPoolAllocator, nPossibleEvents, sizeof(irp*), nullptr);
    size_t unsignaledIRPIndex = 0;
    again:
    if (read_set)
    {
        set_foreach(read_set, 128)
        {
            irp* tmp = nullptr;
            if (fd_avaliable_for(IRP_READ, fd, &status, &tmp))
            {
                // printf("fd %d is ready for reading\n", fd);
                num_events++;
                read_set_tmp[_i] |= BIT(_j);
            }
            else if (!num_events)
                unsignaledIRPs[unsignaledIRPIndex++] = tmp;
            if (obos_is_error(status))
            {
                printf("%d\n", status);
                goto out;
            }
        }
    }
    if (write_set)
    {
        set_foreach(write_set, 128)
        {
            irp* tmp = nullptr;
            if (fd_avaliable_for(IRP_WRITE, fd, &status, &tmp))
            {
                // printf("fd %d is ready for writing\n", fd);
                write_set_tmp[_i] |= BIT(_j);
                num_events++;
            }
            else if (!num_events)
                unsignaledIRPs[unsignaledIRPIndex++] = tmp;
            if (obos_is_error(status))
                goto out;
        }
    }
    
    out:
    (void)0;
    uintptr_t timeout = UINTPTR_MAX;
    if (extra.timeout)
        memcpy_usr_to_k(&timeout, extra.timeout, sizeof(uintptr_t));
    if (!num_events)
    {
        if (!timeout)
        {
            status = OBOS_STATUS_TIMED_OUT;
            goto timeout;
        }

        size_t nWaitableObjects = unsignaledIRPIndex;
        
        if (timeout != UINTPTR_MAX)
            nWaitableObjects++;
        
        struct waitable_header** waitable_list = ZeroAllocate(OBOS_NonPagedPoolAllocator, nWaitableObjects, sizeof(struct waitable_header*), nullptr);
        for (size_t i = 0; i < unsignaledIRPIndex; i++)
            waitable_list[i] = WAITABLE_OBJECT(*unsignaledIRPs[i]->evnt);
        timer tm = {};
        event tm_evnt = {};
        if (timeout != UINTPTR_MAX)
        {
            tm.handler = pselect_tm_handler;
            tm.userdata = (void*)&tm_evnt;
            Core_TimerObjectInitialize(&tm, TIMER_MODE_DEADLINE, timeout);
            waitable_list[unsignaledIRPIndex] = WAITABLE_OBJECT(tm_evnt);
        }

        struct waitable_header* signaled = nullptr;
        Core_WaitOnObjects(nWaitableObjects, waitable_list, &signaled);
        if (tm.mode == TIMER_EXPIRED)
        {
            CoreH_FreeDPC(&tm.handler_dpc, false);
            status = OBOS_STATUS_TIMED_OUT;
            goto timeout;
        }
        Core_CancelTimer(&tm);
        CoreH_FreeDPC(&tm.handler_dpc, false);
        unsignaledIRPIndex = 0;
        Free(OBOS_NonPagedPoolAllocator, waitable_list, nWaitableObjects*sizeof(struct waitable_header*));
        goto again;
    }
    timeout:
    Free(OBOS_NonPagedPoolAllocator, unsignaledIRPs, nPossibleEvents*sizeof(irp*));
    if (extra.sigmask)
        OBOS_SigProcMask(SIG_SETMASK, &oldmask, nullptr);

    if (read_set)
        memcpy(read_set, read_set_tmp, 128);
    if (write_set)
        memcpy(write_set, write_set_tmp, 128);
    if (extra.num_events)
        memcpy_k_to_usr(extra.num_events, &num_events, sizeof(num_events));

    out2:
    Mm_VirtualMemoryFree(CoreS_GetCPULocalPtr()->currentContext, read_set, 128);
    Mm_VirtualMemoryFree(CoreS_GetCPULocalPtr()->currentContext, write_set, 128);
    return status;
}

obos_status Sys_SymLink(const char* target, const char* link)
{
    return Sys_SymLinkAt(target, AT_FDCWD, link);
}

obos_status Sys_SymLinkAt(const char* utarget, handle dirfd, const char* ulink)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    char* target = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(utarget, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    target = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(utarget, target, nullptr);
    
    char* link = nullptr;
    size_t sz_path2 = 0;
    status = OBOSH_ReadUserString(utarget, nullptr, &sz_path2);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, target, sz_path2);
        return status;
    }
    link = ZeroAllocate(OBOS_KernelAllocator, sz_path2+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(ulink, link, nullptr);

    dirent* parent = nullptr;
    const char* link_name = nullptr;
    if (dirfd != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            goto fail;
        }
        parent = desc->un.dirent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

        if (strchr(link, '/') != strlen(link))
        {
            size_t last_slash = strrfind(link, '/');
            char ch = link[last_slash];
            link[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(link, parent);
            link[last_slash] = ch;
            link_name = link+last_slash+1;
        }
        else
            link_name = link;
    }
    else 
    {
        parent = Core_GetCurrentThread()->proc->cwd;
        if (strchr(link, '/') != strlen(link))
        {
            size_t last_slash = strrfind(link, '/');
            char ch = link[last_slash];
            link[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(link, parent);
            link[last_slash] = ch;
            link_name = link+last_slash+1;
        }
        else
            link_name = link;
    }

    if (!parent)
    {
        status = OBOS_STATUS_NOT_FOUND;
        goto fail;
    }

    file_perm perm = {.mode=0777};
    status = Vfs_CreateNode(parent, link_name, VNODE_TYPE_LNK, perm);
    dirent* node = VfsH_DirentLookupFrom(link_name, parent);
    node->vnode->un.linked = target;

    fail:
    Free(OBOS_KernelAllocator, link, sz_path2);
    if (obos_is_error(status))
        Free(OBOS_KernelAllocator, target, sz_path);
    return status;
}

#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

#define F_DUPFD_CLOEXEC 1030
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034

#define FD_CLOEXEC 1

#define O_DIRECT      040000

#define O_RDONLY   00
#define O_WRONLY   01
#define O_RDWR     02

obos_status Sys_Fcntl(handle desc, int request, uintptr_t* uargs, size_t nArgs, int* uret)
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

    uintptr_t* args = Mm_MapViewOfUserMemory(
        CoreS_GetCPULocalPtr()->currentContext, 
        uargs, nullptr, 
        nArgs*sizeof(uintptr_t), 
        0, true, 
        &status);
    if (!args && nArgs)
        return status;

    int res = 0;
    switch (request) {
        case F_GETFD: res = fd->un.fd->flags & FD_FLAGS_NOEXEC ? FD_CLOEXEC : 0; status = OBOS_STATUS_SUCCESS; break;
        case F_SETFD: 
        {
            if (!nArgs)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (args[0] & FD_CLOEXEC)
                fd->un.fd->flags |= FD_FLAGS_NOEXEC;
            else
                fd->un.fd->flags &= ~FD_FLAGS_NOEXEC;
            status = OBOS_STATUS_SUCCESS; 
            break;
        }
        case F_GETFL:
        {
            if ((fd->un.fd->flags & FD_FLAGS_READ) && (~fd->un.fd->flags & FD_FLAGS_WRITE))
                res = O_RDONLY;
            if ((fd->un.fd->flags & FD_FLAGS_READ) && (fd->un.fd->flags & FD_FLAGS_WRITE))
                res = O_RDWR;
            if ((~fd->un.fd->flags & FD_FLAGS_READ) && (fd->un.fd->flags & FD_FLAGS_WRITE))
                res = O_WRONLY;
            if (fd->un.fd->flags & FD_FLAGS_UNCACHED)
                res |= O_DIRECT;
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        case F_SETFL:
        {
            if (!nArgs)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            /*
             * "On Linux, this operation can change only the O_APPEND, O_ASYNC, O_DIRECT, O_NOATIME, and O_NONBLOCK flags.
             * It is not possible to change the O_DSYNC and O_SYNC flags; see BUGS, below.""
            */
            if (args[0] & O_DIRECT)
                fd->un.fd->flags |= FD_FLAGS_UNCACHED;
            else
                fd->un.fd->flags &= ~FD_FLAGS_UNCACHED;
            break;
        }
        case F_DUPFD:
        {
            // doesn't exactly follow linux, but whatever.
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            handle_desc* desc = nullptr;
            handle new_desc = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
            void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[HANDLE_TYPE_FD];
            cb(fd, desc);
            desc->type = HANDLE_TYPE_FD;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            status = OBOS_STATUS_SUCCESS;
            res = new_desc;
            break;
        }
        case F_DUPFD_CLOEXEC:
        {
            // doesn't exactly follow linux, but whatever.
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            handle_desc* desc = nullptr;
            handle new_desc = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &desc);
            void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[HANDLE_TYPE_FD];
            cb(fd, desc);
            desc->un.fd->flags |= FD_FLAGS_NOEXEC;
            desc->type = HANDLE_TYPE_FD;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            res = new_desc;
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        case F_SETPIPE_SZ:
        {
            if (fd->un.fd->vn->vtype != VNODE_TYPE_FIFO || !nArgs)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            size_t curr_size = 0;
            status = Vfs_FdIoctl(fd->un.fd, 2, &curr_size);
            size_t new_size = args[0];
            if (curr_size < new_size)
            {
                // This is weird, but in obos' mlibc sysdep it translates to EBUSY.
                status = OBOS_STATUS_WOULD_BLOCK;
                break;
            }
            status = Vfs_FdIoctl(fd->un.fd, 1, &new_size);
            res = (int)new_size;
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        case F_GETPIPE_SZ:
        {
            if (fd->un.fd->vn->vtype != VNODE_TYPE_FIFO)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            size_t size = 0;
            status = Vfs_FdIoctl(fd->un.fd, 2, &size);
            res = (int)size;
            break;
        }
        case F_ADD_SEALS:
        {
            if (!nArgs)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (args[0] & ~0xf)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            if (fd->un.fd->vn->seals & F_SEAL_SEAL)
            {
                status = OBOS_STATUS_ACCESS_DENIED;
                break;
            }
            if (fd->un.fd->vn->seals & F_SEAL_WRITE && fd->un.fd->vn->nWriteableMappedRegions)
            {
                // This is weird, but in obos' mlibc sysdep it translates to EBUSY.
                status = OBOS_STATUS_WOULD_BLOCK;
                break;
            }
            int seal_mask = args[0] & 0xf;
            fd->un.fd->vn->seals |= seal_mask;
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        case F_GET_SEALS:
        {
            res = fd->un.fd->vn->seals;
            status = OBOS_STATUS_SUCCESS;
            break;
        }
        default: res = 0; status = OBOS_STATUS_INVALID_ARGUMENT; break;
    }

    if (uret)
        memcpy_k_to_usr(uret, &res, sizeof(int));

    Mm_VirtualMemoryFree(&Mm_KernelContext, args, nArgs*sizeof(uintptr_t));

    return status;
}