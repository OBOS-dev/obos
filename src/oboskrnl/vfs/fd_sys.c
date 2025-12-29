/*
 * oboskrnl/vfs/fd_sys.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <signal.h>
#include <perm.h>
#include <klog.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <syscall.h>
#include <partition.h>

#include <allocators/base.h>

#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/sched_sys.h>

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
#include <vfs/socket.h>

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

static file_perm unix_to_obos_mode(uint32_t mode, bool respect_umask)
{
    if (respect_umask)
        mode &= ~Core_GetCurrentThread()->proc->umask;
    // NOTE(oberrow): Whatever happened to warnings about
    // uninitialized variables??
    file_perm real_mode = {};
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
    if (mode & 0100)
        real_mode.owner_exec = true;
    if (mode & 0200)
        real_mode.owner_write = true;
    if (mode & 0400)
        real_mode.owner_read = true;
    if (mode & 04000)
        real_mode.set_uid = true;
    if (mode & 02000)
        real_mode.set_gid = true;
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
            Free(OBOS_KernelAllocator, path, sz_path+1);
            return OBOS_STATUS_NOT_FOUND; // parent wasn't found.
        }
        file_perm real_mode = unix_to_obos_mode(mode, true);
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
    // if (obos_is_error(status))
    //     printf("failed (status=%d) open of %s on fd 0x%x\n", status, path, desc);
    // else
    //     printf("open of %s on fd 0x%x\n", path, desc);
    Free(OBOS_KernelAllocator, path, sz_path+1);
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

    return Vfs_FdOpenDirent(fd->un.fd, dent->un.dirent->parent, oflags & ~FD_OFLAGS_CREATE);
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
        parent_dent = dent->un.dirent->parent;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    char* name = nullptr;
    size_t sz_name = 0;
    status = OBOSH_ReadUserString(uname, nullptr, &sz_name);
    if (obos_is_error(status))
        return status;
    name = ZeroAllocate(OBOS_KernelAllocator, sz_name+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uname, name, nullptr);
    // printf("opening %s\n", name);
    real_dent = VfsH_DirentLookupFrom(name, parent_dent);

    if (!real_dent)
    {
        if (~oflags & FD_OFLAGS_CREATE)
        {
            Free(OBOS_KernelAllocator, name, sz_name);
            return OBOS_STATUS_NOT_FOUND;
        }
        status = Vfs_CreateNode(parent_dent, name, VNODE_TYPE_REG, unix_to_obos_mode(mode, true));
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

    if (!fd->un.fd->vn)
        return OBOS_STATUS_UNINITIALIZED;
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

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_FdRead(handle desc, void* buf, size_t nBytes, size_t* nRead)
{
    // for (volatile bool b = (desc==0); b; )
    //     ;
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
    
    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

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

    if (!fd->un.fd->vn)
        return OBOS_STATUS_UNINITIALIZED;
    if (fd->un.fd->vn->seals & F_SEAL_WRITE)
        return OBOS_STATUS_ACCESS_DENIED;
    if ((fd->un.fd->vn->seals & F_SEAL_GROW) && (offset + nBytes) > fd->un.fd->vn->filesize)
        return OBOS_STATUS_ACCESS_DENIED;

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

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

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
    
    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();
    
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

obos_status Sys_FdIoctl(handle desc, uintptr_t request, void* argp, size_t sz_argp)
{
    // obos_status status = OBOS_CapabilityCheck("fs/ioctl", true);
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status))
        return status;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    if (sz_argp == SIZE_MAX)
    {
        if (!fd->un.fd->vn)
            return OBOS_STATUS_UNINITIALIZED;
        driver_header* header = Vfs_GetVnodeDriver(fd->un.fd->vn);
        if (!header)
            return OBOS_STATUS_INTERNAL_ERROR;
        if (header->ftable.ioctl_argp_size)
            status = header->ftable.ioctl_argp_size(request, &sz_argp);
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
    if (HANDLE_TYPE(desc) == HANDLE_TYPE_DIRENT)
        return OBOS_STATUS_SUCCESS;
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
            if (HANDLE_TYPE(desc) != HANDLE_TYPE_FD && HANDLE_TYPE(desc) != HANDLE_TYPE_DIRENT)
                return OBOS_STATUS_INVALID_ARGUMENT;
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            status = OBOS_STATUS_SUCCESS;
            handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, true, &status);
            if (!fd)
            {
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                return status;
            }
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            if (!fd->un.fd->vn)
                return OBOS_STATUS_UNINITIALIZED;
            to_stat = HANDLE_TYPE(desc) == HANDLE_TYPE_FD ? fd->un.fd->vn : fd->un.dirent->parent->vnode;
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
            dirent* dent = nullptr;
            if (FSFDT_PATH == fsfdt || desc == AT_FDCWD)
                dent = VfsH_DirentLookup(path);
            else
            {
                status = OBOS_STATUS_SUCCESS;
                handle_desc* ent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_DIRENT, false, &status);
                if (!ent)
                {
                    Free(OBOS_KernelAllocator, path, sz_path+1);
                    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                    return status;
                }
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                dent = VfsH_DirentLookupFrom(path, ent->un.dirent->parent);
            }
            // printf("trying stat of %s\n", path);
            Free(OBOS_KernelAllocator, path, sz_path+1);
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
    if (to_stat->perm.set_uid)
        st.st_mode |= 04000;
    if (to_stat->perm.set_gid)
        st.st_mode |= 02000;
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
    if (to_stat->vtype != VNODE_TYPE_CHR && to_stat->vtype != VNODE_TYPE_BLK && to_stat->vtype != VNODE_TYPE_FIFO && to_stat->vtype != VNODE_TYPE_SOCK && (~to_stat->flags & VFLAGS_EVENT_DEV))
    {
        drv_fs_info fs_info = {};
        OBOS_ENSURE (to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info);
        to_stat->mount_point->fs_driver->driver->header.ftable.stat_fs_info(to_stat->mount_point->device, &fs_info);
        st.st_blocks = (to_stat->filesize+(fs_info.fsBlockSize-(to_stat->filesize%fs_info.fsBlockSize)))/512;
        st.st_blksize = fs_info.fsBlockSize;
    }
    st.st_atim.tv_sec = to_stat->times.access;
    st.st_mtim.tv_sec = to_stat->times.change;
    st.st_ctim.tv_sec = to_stat->times.birth;
    st.st_atim.tv_nsec = 0;
    st.st_ctim.tv_nsec = 0;
    st.st_mtim.tv_nsec = 0;
    st.st_gid = to_stat->gid;
    st.st_uid = to_stat->uid;
    st.st_ino = to_stat->inode;   
    if (to_stat->flags & VFLAGS_EVENT_DEV && ~to_stat->flags & VFLAGS_DRIVER_DEAD)
        goto done;
    const driver_header* driver = Vfs_GetVnodeDriverStat(to_stat);
    if (!driver)
        return OBOS_STATUS_INVALID_ARGUMENT;
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
        Free(OBOS_KernelAllocator, path, sz_path+1);
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

        dirent* dent = VfsH_DirentLookupFrom(path, parent_dirent->un.dirent->parent);
        Free(OBOS_KernelAllocator, path, sz_path+1);
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
        Free(OBOS_KernelAllocator, path, sz_path+1);
        if (!dent)
            return OBOS_STATUS_NOT_FOUND;

        vn = dent->vnode;
    }
    else if (!strlen(path))
    {
        Free(OBOS_KernelAllocator, path, sz_path+1);
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
                vn = fd->un.dirent->parent->vnode;
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

    Mm_VirtualMemoryFree(&Mm_KernelContext, buff, max_size);

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

    vnode* vn = dent->un.dirent->parent->vnode;
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
    Free(OBOS_KernelAllocator, path, sz_path+1);
    if (!dent)
    {
        status = OBOS_STATUS_NOT_FOUND;
        if (statusp)
            memcpy_k_to_usr(statusp, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }

    dent = VfsH_FollowLink(dent);

    if (dent->vnode)
    {
        obos_status status = Vfs_Access(dent->vnode, 
                                    true,
                                    false,
                                    false);
        if (obos_is_error(status))
            return status;
    }

    Vfs_PopulateDirectory(dent);
    
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_DIRENT, &desc);
    desc->un.dirent = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*desc->un.dirent), nullptr);
    desc->un.dirent->curr = dent->d_children.head;
    desc->un.dirent->parent = dent;
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

    if (dent->un.dirent->curr == nullptr)
    {
        if (nRead)
            memcpy_k_to_usr(nRead, &k_nRead, sizeof(size_t));
        return OBOS_STATUS_SUCCESS;
    }

    void* kbuff = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer, nullptr, szBuf, 0, true, &status);
    if (!kbuff)
        return status;

    memzero(kbuff, szBuf);

    status = Vfs_ReadEntries(dent->un.dirent->curr, kbuff, szBuf, &dent->un.dirent->curr, nRead ? &k_nRead : nullptr);
    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuff, szBuf);
    if (obos_is_error(status))
        return status;

    if (nRead)
        memcpy_k_to_usr(nRead, &k_nRead, sizeof(size_t));

    return status;
}

obos_status Sys_Mkdir(const char* upath, uint32_t mode)
{
    return Sys_MkdirAt(AT_FDCWD, upath, mode);
}
obos_status Sys_MkdirAt(handle ent, const char* uname, uint32_t mode)
{
    handle_desc* dent = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (ent != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), ent, HANDLE_TYPE_DIRENT, false, &status);
        if (!dent)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }

    char* name = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(uname, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    name = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uname, name, nullptr);

    dirent* parent = !dent ? Core_GetCurrentThread()->proc->cwd : dent->un.dirent->parent;
    if (name[0] == '/')
        parent = Vfs_Root;
    size_t index = strrfind(name, '/');
    char* dirname = name;
    if (index != SIZE_MAX)
    {
        dirname = name+index;
        *dirname = 0;
        dirname++;
        parent = VfsH_DirentLookupFrom(name, parent);
    }
    if (VfsH_DirentLookupFrom(dirname, parent))
        return OBOS_STATUS_ALREADY_INITIALIZED;

    status = Vfs_CreateNode(parent, dirname, VNODE_TYPE_DIR, unix_to_obos_mode(mode, true));
    Free(OBOS_KernelAllocator, name, sz_path+1);

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
    if (!Core_GetCurrentThread()->proc->pgrp)
    {
        OBOS_Warning("%s: No PGRP for current process\n", __func__);
        return;
    }
    if (!Core_GetCurrentThread()->proc->pgrp->controlling_tty)
    {
        OBOS_Warning("%s: No controlling tty\n", __func__);
        return;
    }
    handle hnd_stdin = alloc_fd(tbl);
    handle hnd_stdout = alloc_fd(tbl);
    handle hnd_stderr = alloc_fd(tbl);
    OBOS_LockHandleTable(tbl);
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* stdin = OBOS_HandleLookup(tbl, hnd_stdin, HANDLE_TYPE_FD, false, &status);
    handle_desc* stdout = OBOS_HandleLookup(tbl, hnd_stdout, HANDLE_TYPE_FD, false, &status);
    handle_desc* stderr = OBOS_HandleLookup(tbl, hnd_stderr, HANDLE_TYPE_FD, false, &status);
    OBOS_UnlockHandleTable(tbl);
    Vfs_FdOpenVnode(stdin->un.fd, Core_GetCurrentThread()->proc->pgrp->controlling_tty->vn, FD_OFLAGS_READ);
    Vfs_FdOpenVnode(stdout->un.fd, Core_GetCurrentThread()->proc->pgrp->controlling_tty->vn, FD_OFLAGS_WRITE);
    Vfs_FdOpenVnode(stderr->un.fd, Core_GetCurrentThread()->proc->pgrp->controlling_tty->vn, FD_OFLAGS_WRITE);
}

// Writebacks all dirty pages in the page cache back to disk.
// Do this twice, in case a file gets flushed, and then the filesystem driver
// makes new dirty pages.
void Sys_Sync()
{
    obos_status status = OBOS_CapabilityCheck("fs/sync", true);
    if (obos_is_error(status))
        return;

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

    obos_status status = OBOS_CapabilityCheck("fs/mount", false);
    if (obos_is_error(status))
        return status;

    char* at = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(uat, nullptr, &sz_path);
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
    obos_status status = OBOS_CapabilityCheck("fs/unmount", false);
    if (obos_is_error(status))
        return status;

    char* at = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(uat, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    at = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uat, at, nullptr);

    status = Vfs_UnmountP(at);

    Free(OBOS_KernelAllocator, at, sz_path);

    return status;
}

obos_status Sys_IRPCreate(handle *ufile, size_t offset, size_t size, enum irp_op operation, void* buffer)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    switch (operation) {
        case IRP_READ:
        case IRP_WRITE:
            break;
        default:
            status = OBOS_STATUS_INVALID_ARGUMENT;
            return status;
    }

    handle file = 0;
    status = memcpy_usr_to_k(&file, ufile, sizeof(handle));
    if (obos_is_error(status))
        return status;

    vnode* vn = nullptr;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), file, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    vn = fd->un.fd->vn;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    if (!vn)
        return OBOS_STATUS_UNINITIALIZED;

    bool dry = !buffer;
    void* buff = dry ? nullptr : Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, buffer, nullptr, size, operation == IRP_READ ? 0 : OBOS_PROTECTION_READ_ONLY, true, &status);

    user_irp* obj = nullptr;
    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_IRP, &desc);
    desc->un.irp = obj = Vfs_Calloc(1, sizeof(user_irp));
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    obj->obj = VfsH_IRPAllocate();
    obj->buff_size = size;
    
    VfsH_IRPBytesToBlockCount(vn, size, &obj->obj->blkCount);
    VfsH_IRPBytesToBlockCount(vn, offset, &obj->obj->blkOffset);
    obj->obj->op = operation;
    obj->obj->dryOp = dry;
    obj->obj->buff = buff;
    obj->obj->vn = vn;
    obj->obj->status = OBOS_STATUS_SUCCESS;
    obj->desc = fd->un.fd->desc;

    return memcpy_k_to_usr(ufile, &ret, sizeof(ret));
}
obos_status Sys_IRPSubmit(handle desc)
{
    obos_status status = OBOS_STATUS_SUCCESS;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_IRP, false, &status);
    if (!irph)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return VfsH_IRPSubmit(irph->un.irp->obj, &irph->un.irp->desc);
}

obos_status Sys_IRPWait(handle desc, obos_status* out_status, size_t* nCompleted /* irp.nBlkRead/nBlkWritten */, bool close)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    
    if (!out_status && !nCompleted)
        return OBOS_STATUS_INVALID_ARGUMENT;

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_IRP, false, &status);
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

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_IRP, false, &status);
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

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_IRP, false, &status);
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

    handle_desc* irph = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_IRP, false, &status);
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
    Vfs_FdOpenVnode(tmp_descs[0]->un.fd, kfds[0].vn, FD_OFLAGS_READ);
    Vfs_FdOpenVnode(tmp_descs[1]->un.fd, kfds[1].vn, FD_OFLAGS_WRITE);
    Vfs_FdClose(&kfds[0]);
    Vfs_FdClose(&kfds[1]);

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
        Free(OBOS_KernelAllocator, path, sz_path+1);
        return status;
    }
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);

    file_perm perm = unix_to_obos_mode(mode, true);
    const char* fifo_name = nullptr;

    dirent* parent = nullptr;
    if (dirfd != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            Free(OBOS_KernelAllocator, path, sz_path+1);
            return status;
        }
        parent = desc->un.dirent->parent;
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

    gid current_gid = Core_GetCurrentThread()->proc->egid;
    uid current_uid = Core_GetCurrentThread()->proc->euid;
    status = Vfs_CreateNamedPipe(perm, current_gid, current_uid, parent, fifo_name, pipesize);
    Free(OBOS_KernelAllocator, path, sz_path+1);
    
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
    if (~fd->un.fd->flags & FD_FLAGS_OPEN)
    {
        if (status) *status = OBOS_STATUS_UNINITIALIZED;
        return false;
    }

    irp* req = VfsH_IRPAllocate();
    req->dryOp = true;
    req->op = op;
    req->vn = fd->un.fd->vn;
    req->blkCount = 1;
    VfsH_IRPBytesToBlockCount(req->vn, fd->un.fd->offset, &req->blkOffset);
    *status = VfsH_IRPSubmit(req, &fd->un.fd->desc);
    if (obos_is_error(*status))
    {
        VfsH_IRPUnref(req);
        return false;
    }
    bool res = !req->evnt;
    if (req->evnt && req->evnt->hdr.signaled)
        res = true;
    // if (!res)
    *oreq = req;
    // else
    // {
    //     VfsH_IRPUnref(req);
    //     *oreq = nullptr;
    // }
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
    if (uexcept_set)
        return OBOS_STATUS_UNIMPLEMENTED;

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
            else if (!num_events && tmp)
                unsignaledIRPs[unsignaledIRPIndex++] = tmp;
            if (obos_is_error(status))
                goto out;
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
            else if (!num_events && tmp)
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
    if (!num_events && obos_is_success(status))
    {
        if (!timeout)
        {
            status = OBOS_STATUS_SUCCESS;
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

        status = Core_WaitOnObjects(nWaitableObjects, waitable_list, nullptr);
        if (obos_is_error(status))
        {
            Core_CancelTimer(&tm);
            CoreH_FreeDPC(&tm.handler_dpc, false);
            goto timeout;
        }
        if (tm.mode == TIMER_EXPIRED)
        {
            // Just In Case
            Core_CancelTimer(&tm);
            CoreH_FreeDPC(&tm.handler_dpc, false);
            status = OBOS_STATUS_SUCCESS;
            goto timeout;
        }
        Core_CancelTimer(&tm);
        CoreH_FreeDPC(&tm.handler_dpc, false);
        unsignaledIRPIndex = 0;
        Free(OBOS_NonPagedPoolAllocator, waitable_list, nWaitableObjects*sizeof(struct waitable_header*));
        goto again;
    }
    timeout:
    for (size_t i = 0; i < nPossibleEvents; i++)
        if (unsignaledIRPs[i])
            VfsH_IRPUnref(unsignaledIRPs[i]);
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

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return status;
}

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200
#define POLLRDHUP 0x2000

obos_status Sys_PPoll(struct pollfd* ufds, size_t nFds, const uintptr_t* utimeout, const sigset_t* usigmask, int *nEvents)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    struct pollfd* fds = Mm_MapViewOfUserMemory(
        CoreS_GetCPULocalPtr()->currentContext, 
        ufds, nullptr,
        nFds*sizeof(struct pollfd), 
        0, true, 
        &status);
    if (!fds)
        return status;

    const event set_event = { .hdr=WAITABLE_HEADER_INITIALIZE(true, true), .type=(EVENT_NOTIFICATION) };

    sigset_t sigmask = 0, oldmask = 0;
    if (usigmask)
    {
        status = memcpy_usr_to_k(&sigmask, usigmask, sizeof(sigset_t));
        if (obos_is_error(status))
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, fds, nFds*sizeof(struct pollfd));
            return status;
        }
        OBOS_SigProcMask(SIG_SETMASK, &sigmask, &oldmask);
    }
    
    uintptr_t timeout = UINTPTR_MAX;
    if (utimeout)
    {
        status = memcpy_usr_to_k(&timeout, utimeout, sizeof(uintptr_t));
        if (obos_is_error(status))
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, fds, nFds*sizeof(struct pollfd));
            return status;
        }
    }

    int num_events = 0;
    int nTotalEvents = 0;

    for (size_t i = 0; i < nFds; i++)
    {
        struct pollfd* curr = &fds[i];
        // TODO: is this a problem with different handle types?
        if ((int)curr->fd < 0)
            continue;
        nTotalEvents += (curr->events & POLLIN);
        nTotalEvents += (curr->events & POLLOUT);
    }
    nTotalEvents += (timeout != UINTPTR_MAX);
    struct waitable_header** waitable_list = ZeroAllocate(OBOS_NonPagedPoolAllocator, nTotalEvents, sizeof(struct waitable_header*), nullptr);
    size_t n_waitable_objects = 0;
    irp** irp_list = nullptr;
    size_t irp_count = 0;

    // FIXME: The IRPs are leaked
    // FYM THE IRPS ARE LEAKED U DUMBASS
    
    up:
    for (size_t i = 0; i < nFds; i++)
    {
        struct pollfd* curr = &fds[i];
        // TODO: is this a problem with different handle types?
        if ((int)curr->fd < 0)
            continue;
        irp* read_irp = nullptr;
        irp* write_irp = nullptr;
        bool events_satisified = true;
        curr->revents = 0;
        if (curr->events & POLLIN)
        {
            bool res = fd_avaliable_for(IRP_READ, curr->fd, &status, &read_irp);
            events_satisified = events_satisified && res;
            if (obos_is_error(status))
            {
                if (status == OBOS_STATUS_PIPE_CLOSED)
                {
                    curr->revents |= POLLERR;
                    status = OBOS_STATUS_SUCCESS;
                }
                else if (status == OBOS_STATUS_INVALID_ARGUMENT)
                {
                    curr->revents |= POLLNVAL;
                    status = OBOS_STATUS_SUCCESS;
                }
                else
                    break;
            }
            if (res)
            {
                num_events++;
                curr->revents |= POLLIN;
            }
        }
        if (curr->events & POLLOUT)
        {
            bool res = fd_avaliable_for(IRP_WRITE, curr->fd, &status, &write_irp);
            events_satisified = events_satisified && res;
            if (obos_is_error(status))
            {
                if (status == OBOS_STATUS_PIPE_CLOSED)
                {
                    curr->revents |= POLLERR;
                    status = OBOS_STATUS_SUCCESS;
                }
                else if (status == OBOS_STATUS_INVALID_ARGUMENT)
                {
                    curr->revents |= POLLNVAL;
                    status = OBOS_STATUS_SUCCESS;
                }
                else
                    break;
            }
            if (res)
            {
                num_events++;
                curr->revents |= POLLOUT;
            }
        }
        if (read_irp)
        {
            if (!events_satisified)
            {
                if (read_irp->evnt)
                    waitable_list[n_waitable_objects++] = WAITABLE_OBJECT(*read_irp->evnt);
                else
                    waitable_list[n_waitable_objects++] = WAITABLE_OBJECT(set_event);
            }
            irp_count++;
            irp_list = Reallocate(OBOS_NonPagedPoolAllocator, irp_list, irp_count*sizeof(irp*), (irp_count-1)*sizeof(irp*),nullptr);
            irp_list[irp_count-1] = read_irp;
        }
        if (write_irp)
        {
            if (!events_satisified)
            {
                if (write_irp->evnt)
                    waitable_list[n_waitable_objects++] = WAITABLE_OBJECT(*write_irp->evnt);
                else
                    waitable_list[n_waitable_objects++] = WAITABLE_OBJECT(set_event);
            }
            irp_count++;
            irp_list = Reallocate(OBOS_NonPagedPoolAllocator, irp_list, irp_count*sizeof(irp*), (irp_count-1)*sizeof(irp*),nullptr);
            irp_list[irp_count-1] = write_irp;
        }
    }

    if (n_waitable_objects == nFds && obos_is_success(status))
    {
        if (!timeout)
        {
            status = OBOS_STATUS_SUCCESS;
            goto out;
        }

        timer tm = {};
        event tm_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        if (timeout != UINTPTR_MAX)
        {
            tm.handler = pselect_tm_handler;
            tm.userdata = (void*)&tm_evnt;
            Core_TimerObjectInitialize(&tm, TIMER_MODE_DEADLINE, timeout);
            waitable_list[n_waitable_objects++] = WAITABLE_OBJECT(tm_evnt);
        }

        struct waitable_header* signaled = nullptr;
        status = Core_WaitOnObjects(n_waitable_objects, waitable_list, &signaled);
        if (obos_is_error(status))
        {
            Core_CancelTimer(&tm);
            CoreH_FreeDPC(&tm.handler_dpc, false);
            goto out;
        }
        if (signaled == WAITABLE_OBJECT(tm_evnt))
        {
            Core_CancelTimer(&tm);
            CoreH_FreeDPC(&tm.handler_dpc, false);
            status = OBOS_STATUS_SUCCESS;
            goto out;
        }
        Core_CancelTimer(&tm);
        CoreH_FreeDPC(&tm.handler_dpc, false);

        n_waitable_objects = 0;
        for (size_t i = 0; i < irp_count; i++)
            VfsH_IRPUnref(irp_list[i]);
        Free(OBOS_NonPagedPoolAllocator, irp_list, irp_count*sizeof(irp*));
        irp_list = nullptr;
        irp_count = 0;
        goto up;
    }

    out:

    if (irp_list)
    {
        for (size_t i = 0; i < irp_count; i++)
            VfsH_IRPUnref(irp_list[i]);
        Free(OBOS_NonPagedPoolAllocator, irp_list, irp_count*sizeof(irp*));
    }

    memcpy_k_to_usr(nEvents, &num_events, sizeof(int));

    if (waitable_list)
        Free(OBOS_NonPagedPoolAllocator, waitable_list, nTotalEvents*sizeof(struct waitable_header*));
    OBOS_SigProcMask(SIG_SETMASK, &oldmask, nullptr);
    Mm_VirtualMemoryFree(&Mm_KernelContext, fds, nFds*sizeof(struct pollfd));

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

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
    status = OBOSH_ReadUserString(ulink, nullptr, &sz_path2);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
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
        parent = desc->un.dirent->parent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

        if (strchr(link, '/') != strlen(link))
        {
            size_t last_slash = strrfind(link, '/');
            char ch = link[last_slash];
            link[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(link, *link == '/' ? Vfs_Root : parent);
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
            parent = VfsH_DirentLookupFrom(link, *link == '/' ? Vfs_Root : parent);
            link[last_slash] = ch;
            link_name = link+last_slash+1;
        }
        else
            link_name = link;
    }

    if (*link == '/')
        parent = Vfs_Root;

    if (!parent)
    {
        status = OBOS_STATUS_NOT_FOUND;
        goto fail;
    }

    if (VfsH_DirentLookupFrom(link_name, parent))
    {
        status = OBOS_STATUS_ALREADY_INITIALIZED;
        goto fail;
    }

    file_perm perm = {.mode=0777};
    status = Vfs_CreateNode(parent, link_name, VNODE_TYPE_LNK, perm);
    dirent* node = VfsH_DirentLookupFrom(link_name, parent);
    node->vnode->un.linked = target;

    fail:
    Free(OBOS_KernelAllocator, link, sz_path+1);
    if (obos_is_error(status))
        Free(OBOS_KernelAllocator, target, sz_path+1);
    return status;
}

obos_status Sys_LinkAt(handle olddirfd, const char *utarget, handle newdirfd, const char *ulink, int flags)
{
    obos_status status = OBOS_CapabilityCheck("fs/hardlink", false);
    if (obos_is_error(status))
        return status;

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

    vnode* vtarget = nullptr;
    dirent* dtarget = nullptr;
    dirent* ptarget = nullptr;

    if (olddirfd == AT_FDCWD)
        ptarget = Core_GetCurrentThread()->proc->cwd;
    else
    {
        if (flags & AT_EMPTY_PATH && !strlen(target))
        {
            if (HANDLE_TYPE(olddirfd) != HANDLE_TYPE_DIRENT && HANDLE_TYPE(olddirfd) != HANDLE_TYPE_FD)
            {
                Free(OBOS_KernelAllocator, target, sz_path+1);
                Free(OBOS_KernelAllocator, link, sz_path+1);
                return OBOS_STATUS_INVALID_ARGUMENT;
            }
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            obos_status status = OBOS_STATUS_SUCCESS;
            handle_desc* hnd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), olddirfd, 0, true, &status);
            if (!hnd)
            {
                Free(OBOS_KernelAllocator, target, sz_path+1);
                Free(OBOS_KernelAllocator, link, sz_path2+1);
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                return status;
            }
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            switch (HANDLE_TYPE(olddirfd)) {
                case HANDLE_TYPE_FD: 
                    if (~hnd->un.fd->flags & FD_FLAGS_OPEN)
                    {
                        Free(OBOS_KernelAllocator, target, sz_path+1);
                        Free(OBOS_KernelAllocator, link, sz_path+1);
                        return OBOS_STATUS_UNINITIALIZED;
                    }
                    vtarget = hnd->un.fd->vn;
                    break;
                case HANDLE_TYPE_DIRENT: 
                    if (!hnd->un.dirent || !hnd->un.dirent->parent)
                    {
                        Free(OBOS_KernelAllocator, target, sz_path+1);
                        Free(OBOS_KernelAllocator, link, sz_path+1);
                        return OBOS_STATUS_UNINITIALIZED;
                    }
                    vtarget = hnd->un.dirent->parent->vnode;
                    break;
                default: OBOS_ENSURE(!"what");
            }
            goto skip_target_lookup;
        }
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), olddirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (!dent)
        {
            Free(OBOS_KernelAllocator, target, sz_path+1);
            Free(OBOS_KernelAllocator, link, sz_path+1);
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        ptarget = dent->un.dirent->parent;
    }

    if (*target == '/')
        ptarget = Vfs_Root;

    dtarget = VfsH_DirentLookupFrom(target, ptarget);
    if (!dtarget)
        return OBOS_STATUS_NOT_FOUND;
    if (flags & AT_SYMLINK_FOLLOW)
        dtarget = VfsH_FollowLink(dtarget);
    vtarget = dtarget->vnode;

    skip_target_lookup:

    Free(OBOS_KernelAllocator, target, sz_path+1);

    if (!vtarget)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }

    dirent* plink = nullptr;

    if (newdirfd != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        status = OBOS_STATUS_SUCCESS;
        handle_desc* hnd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), newdirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (!hnd)
        {
            Free(OBOS_KernelAllocator, link, sz_path+1);
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        plink = hnd->un.dirent->parent;
    }
    else
        plink = *link == '/' ? Vfs_Root : Core_GetCurrentThread()->proc->cwd;

    char* linkname = nullptr;

    if (strchr(link, '/') != strlen(link))
    {
        // frick whoever gave us this path because now i need to parse it >:(
        linkname = strrfind(link, '/') + link;
        *linkname = 0;
        linkname++;
        plink = VfsH_DirentLookupFrom(link, plink);
    }
    else
        linkname = link;

    if (!plink)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_NOT_FOUND;
    }

    // Now we have plink, linkname, target, and vtarget
    driver_header* target_header = Vfs_GetVnodeDriver(vtarget);
    mount* target_mount = Vfs_GetVnodeMount(vtarget);
    mount* link_mount = Vfs_GetVnodeMount(plink->vnode);
    if (!link_mount || !target_mount || !target_header)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    if (target_mount != link_mount)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_ACCESS_DENIED; // well we should return EXDEV but that doesn't exist here does it :)
    }
    if (VfsH_DirentLookupFrom(linkname, plink))
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_ALREADY_INITIALIZED;
    }
    
    // Now for the magic..?
    if (!target_header->ftable.hardlink_file)
        // "EPERM - The filesystem containing oldpath and newpath does not support the creation of hard links."
        status = OBOS_STATUS_ACCESS_DENIED; 
    else
        status = target_header->ftable.hardlink_file(vtarget->desc, plink->vnode->desc, linkname);
    if (obos_is_success(status))
    {
        dirent* ent = Vfs_Calloc(1, sizeof(dirent));
        OBOS_InitString(&ent->name, linkname);
        ent->vnode = vtarget;
        VfsH_DirentAppendChild(plink, ent);
    }

    Free(OBOS_KernelAllocator, link, sz_path+1);

    // NOTE: DO NOT COMMIT!!!!
    return OBOS_STATUS_SUCCESS;

    return status;
}

// this probably works rofl i literally just copy pasted sys linkat and changed some names
obos_status Sys_RenameAt(handle olddirfd, const char *uoldname, handle newdirfd, const char *newname)
{
    char* target = nullptr;
    size_t sz_path = 0;
    obos_status status = OBOSH_ReadUserString(uoldname, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    target = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uoldname, target, nullptr);
    
    char* link = nullptr;
    size_t sz_path2 = 0;
    status = OBOSH_ReadUserString(uoldname, nullptr, &sz_path2);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, target, sz_path2);
        return status;
    }
    link = ZeroAllocate(OBOS_KernelAllocator, sz_path2+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(newname, link, nullptr);

    dirent* dtarget = nullptr;
    dirent* ptarget = nullptr;

    if (olddirfd == AT_FDCWD)
        ptarget = Core_GetCurrentThread()->proc->cwd;
    else
    {
        if (!strlen(target))
        {
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            obos_status status = OBOS_STATUS_SUCCESS;
            handle_desc* hnd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), olddirfd, HANDLE_TYPE_DIRENT, false, &status);
            if (!hnd)
            {
                Free(OBOS_KernelAllocator, target, sz_path2);
                Free(OBOS_KernelAllocator, link, sz_path+1);
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                return status;
            }
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            dtarget = hnd->un.dirent->parent;
            goto skip_target_lookup;
        }
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), olddirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (!dent)
        {
            Free(OBOS_KernelAllocator, target, sz_path+1);
            Free(OBOS_KernelAllocator, link, sz_path+1);
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        ptarget = dent->un.dirent->parent;
    }

    if (*target == '/')
        ptarget = Vfs_Root;

    dtarget = VfsH_DirentLookupFrom(target, ptarget);
    if (!dtarget)
    {
        Free(OBOS_KernelAllocator, target, sz_path+1);
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_NOT_FOUND;
    }
    
    skip_target_lookup:

    Free(OBOS_KernelAllocator, target, sz_path+1);

    if (!dtarget)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }

    dirent* pnewname = nullptr;

    if (newdirfd != AT_FDCWD)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        status = OBOS_STATUS_SUCCESS;
        handle_desc* hnd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), newdirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (!hnd)
        {
            Free(OBOS_KernelAllocator, link, sz_path+1);
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        pnewname = hnd->un.dirent->parent;
    }
    else
        pnewname = Core_GetCurrentThread()->proc->cwd;

    if (*link == '/')
        pnewname = Vfs_Root;

    char* newfilename = nullptr;

    if (strchr(link, '/') != strlen(link))
    {
        // frick whoever gave us this path because now i need to parse it >:(
        newfilename = strrfind(link, '/') + link;
        *newfilename = 0;
        newfilename++;
        pnewname = VfsH_DirentLookupFrom(link, pnewname);
    }
    else
        newfilename = link;

    if (!pnewname)
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_NOT_FOUND;
    }

    // Now we have pnewname, newfilename, target, and vtarget
    if (VfsH_DirentLookupFrom(newfilename, pnewname))
    {
        Free(OBOS_KernelAllocator, link, sz_path+1);
        return OBOS_STATUS_ALREADY_INITIALIZED;
    }
    
    status = Vfs_RenameNode(dtarget, pnewname, newfilename);

    Free(OBOS_KernelAllocator, link, sz_path+1);

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
#define O_NONBLOCK     04000

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
    if (~fd->un.fd->flags & FD_FLAGS_OPEN)
        return OBOS_STATUS_UNINITIALIZED;
    
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
            else if (fd->un.fd->vn->vtype != VNODE_TYPE_CHR && fd->un.fd->vn->vtype != VNODE_TYPE_FIFO && fd->un.fd->vn->vtype != VNODE_TYPE_SOCK)
                fd->un.fd->flags &= ~FD_FLAGS_UNCACHED;
            if (args[0] & O_NONBLOCK)
                fd->un.fd->flags |= FD_FLAGS_NOBLOCK;
            else
                fd->un.fd->flags &= ~FD_FLAGS_NOBLOCK;
            break;
        }
        case F_DUPFD:
        {
            // doesn't exactly follow linux, but whatever.
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            handle_desc* descp = nullptr;
            handle new_desc = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &descp);
            fd = OBOS_CurrentHandleTable()->arr + desc;
            void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[HANDLE_TYPE_FD];
            cb(fd, descp);
            descp->type = HANDLE_TYPE_FD;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            status = OBOS_STATUS_SUCCESS;
            res = new_desc;
            break;
        }
        case F_DUPFD_CLOEXEC:
        {
            // doesn't exactly follow linux, but whatever.
            OBOS_LockHandleTable(OBOS_CurrentHandleTable());
            handle_desc* pdesc = nullptr;
            handle new_desc = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_FD, &pdesc);
            fd = OBOS_CurrentHandleTable()->arr + desc;
            void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[HANDLE_TYPE_FD];
            cb(fd, pdesc);
            pdesc->un.fd->flags |= FD_FLAGS_NOEXEC;
            pdesc->type = HANDLE_TYPE_FD;
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
            if (new_size < curr_size)
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

#ifdef __x86_64__
#   include <arch/x86_64/cmos.h>
#endif

static long get_current_time()
{
    long current_time = 0;
#ifdef __x86_64__
    Arch_CMOSGetEpochTime(&current_time);
#endif
    return current_time;
}

obos_status Sys_UTimeNSAt(handle dirfd, const char *upathname, const struct timespec *utimes, int flags)
{
    struct timespec times[2] = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    
    if (utimes)
    {
        status = memcpy_usr_to_k(times, utimes, sizeof(times));
        if (obos_is_error(status))
            return status;
    }
    else
    {
        times[0].tv_sec = get_current_time();
        times[1].tv_sec = times[0].tv_sec;
    }

    vnode* target = nullptr;
    dirent* parent = nullptr;

    if (dirfd != AT_FDCWD)
    {
        if (flags & AT_EMPTY_PATH || !upathname)
        {
            handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, HANDLE_TYPE_FD, false, &status);
            if (!desc)
            {
                OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
                return status;
            }
            target = desc->un.fd->vn;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            goto have_target;
        }
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, HANDLE_TYPE_DIRENT, false, &status);
        if (!desc)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        parent = desc->un.dirent->parent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
    }
    else
        parent = Core_GetCurrentThread()->proc->cwd;

    char* pathname = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upathname, nullptr, &sz_path);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, target, sz_path+1);
        return status;
    }
    pathname = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upathname, pathname, nullptr);

    dirent* ent = VfsH_DirentLookupFrom(pathname, *pathname == '/' ? Vfs_Root : parent);
    
    Free(OBOS_KernelAllocator, pathname, sz_path+1);

    if (~flags & AT_SYMLINK_NOFOLLOW)
        ent = VfsH_FollowLink(ent);

    if (!ent)
        return OBOS_STATUS_NOT_FOUND;

    target = ent->vnode;
    have_target:
    if (!target)
        return OBOS_STATUS_NOT_FOUND;

    target->times.access = times[0].tv_sec;
    target->times.change = times[1].tv_sec;

    return Vfs_UpdateFileTime(target);
}

obos_status Sys_Socket(handle desc, int family, int type, int protocol)
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

    return Net_Socket(family, type, protocol, fd->un.fd);    
}

obos_status Sys_SendTo(handle desc, const void* buffer, size_t size, int flags, struct sys_socket_io_params *uparams)
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

    struct sys_socket_io_params *params = 
        Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)uparams, nullptr, sizeof(*params), 0, true, &status);
    if (obos_is_error(status))
        return status;
    if (params->sock_addr && (!params->addr_length || params->addr_length > 32))
    {
        status = OBOS_STATUS_INVALID_ARGUMENT;
        goto fail1;
    }

    status = OBOS_STATUS_SUCCESS;
    void* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)buffer, nullptr, size, OBOS_PROTECTION_READ_ONLY, true, &status);
    if (obos_is_error(status))
        return status;

    void* buf = params->addr_length ? Allocate(OBOS_KernelAllocator, params->addr_length, nullptr) : nullptr;
    sockaddr *addr = buf;
    if (params->addr_length)
    {
        status = memcpy_usr_to_k(addr, params->sock_addr, params->addr_length);
        if (obos_is_error(status))
            goto fail3;
    }

    status = Net_SendTo(fd->un.fd, kbuf, size, flags, &params->nWritten, addr, params->addr_length);

    OBOS_MAYBE_UNUSED fail3:
    if (params->addr_length)
        Free(OBOS_KernelAllocator, buf, params->addr_length);
    OBOS_MAYBE_UNUSED fail2:
    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, size);
    OBOS_MAYBE_UNUSED fail1:
    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)params, sizeof(*params));

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return status;
}

obos_status Sys_RecvFrom(handle desc, void* buffer, size_t size, int flags, struct sys_socket_io_params *uparams)
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

    struct sys_socket_io_params *params = 
        Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)uparams, nullptr, sizeof(*params), 0, true, &status);
    if (obos_is_error(status))
        return status;
    if (!params->addr_length && params->sock_addr)
    {
        status = OBOS_STATUS_INVALID_ARGUMENT;
        goto fail1;
    }

    status = OBOS_STATUS_SUCCESS;
    void* kbuf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)buffer, nullptr, size, 0, true, &status);
    if (obos_is_error(status))
        return status;

    void* buf = Allocate(OBOS_KernelAllocator, params->addr_length, nullptr);
    sockaddr *addr = buf;
    status = memcpy_usr_to_k(addr, params->sock_addr, params->addr_length);
    if (obos_is_error(status) && params->sock_addr)
        goto fail3;
    if (!params->sock_addr)
    {
        Free(OBOS_KernelAllocator, buf, params->addr_length);
        addr = nullptr;
    }

    status = Net_RecvFrom(fd->un.fd, kbuf, size, flags, &params->nRead, addr, params->addr_length ? &params->addr_length : nullptr);

    memcpy_k_to_usr(params->sock_addr, addr, params->addr_length);

    OBOS_MAYBE_UNUSED fail3:
    Free(OBOS_KernelAllocator, buf, params->addr_length);
    OBOS_MAYBE_UNUSED fail2:
    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, size);
    OBOS_MAYBE_UNUSED fail1:
    Mm_VirtualMemoryFree(&Mm_KernelContext, params, sizeof(*params));

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return status;
}

obos_status Sys_Listen(handle desc, int backlog)
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

    return Net_Listen(fd->un.fd, backlog);
}

obos_status Sys_Accept(handle desc, handle empty_fd, sockaddr* uaddr_ptr, size_t* uaddr_length, int flags)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_FD, false, &status);
    if (!fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    
    status = OBOS_STATUS_SUCCESS;
    handle_desc* new_fd = OBOS_HandleLookup(OBOS_CurrentHandleTable(), empty_fd, HANDLE_TYPE_FD, false, &status);
    if (!new_fd)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    size_t addr_length = 0;
    if (uaddr_length)
    {
        status = memcpy_usr_to_k(&addr_length, uaddr_length, sizeof(size_t));
        if (obos_is_error(status))
            return status;
    }
    size_t initial_addr_length = addr_length;

    void* buf = uaddr_ptr ? Allocate(OBOS_KernelAllocator, addr_length, nullptr) : nullptr;
    sockaddr *addr = uaddr_ptr ? buf : nullptr;
    if (uaddr_ptr)
    {
        status = memcpy_usr_to_k(addr, uaddr_ptr, addr_length);
        if (obos_is_error(status))
            goto fail1;
    }

    status = Net_Accept(fd->un.fd, addr, uaddr_length ? &addr_length : 0, flags, new_fd->un.fd);

    if (addr_length)
        memcpy_k_to_usr(uaddr_length, &addr_length, sizeof(addr_length));

    OBOS_MAYBE_UNUSED fail1:
    if (buf)
        Free(OBOS_KernelAllocator, buf, initial_addr_length);

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return status;
}

obos_status Sys_Bind(handle desc, const sockaddr *uaddr, size_t addr_length)
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

    void* buf = Allocate(OBOS_KernelAllocator, addr_length, nullptr);
    sockaddr *addr = buf;
    status = memcpy_usr_to_k(addr, uaddr, addr_length);
    if (obos_is_error(status))
        goto fail1;

    status = Net_Bind(fd->un.fd, addr, &addr_length);

    OBOS_MAYBE_UNUSED fail1:
    Free(OBOS_KernelAllocator, buf, addr_length);

    return status;
}

obos_status Sys_Connect(handle desc, const sockaddr *uaddr, size_t addr_length)
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

    void* buf = Allocate(OBOS_KernelAllocator, addr_length, nullptr);
    sockaddr *addr = buf;
    status = memcpy_usr_to_k(addr, uaddr, addr_length);
    if (obos_is_error(status))
        goto fail1;

    status = Net_Connect(fd->un.fd, addr, &addr_length);

    OBOS_MAYBE_UNUSED fail1:
    Free(OBOS_KernelAllocator, buf, addr_length);

    if (CoreS_ForceYieldOnSyscallReturn)
        CoreS_ForceYieldOnSyscallReturn();

    return status;
}

obos_status Sys_SockName(handle desc, sockaddr* uaddr, size_t addr_length, size_t* actual_addr_length)
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

    size_t const addr_length_user = addr_length;
    void* buf = Allocate(OBOS_KernelAllocator, addr_length, nullptr);
    sockaddr *addr = buf;
    status = memcpy_usr_to_k(addr, uaddr, addr_length);
    if (obos_is_error(status))
        goto fail1;

    status = Net_GetSockName(fd->un.fd, addr, &addr_length);

    if (obos_is_success(status))
    {
        memcpy_k_to_usr(actual_addr_length, &addr_length, sizeof(addr_length));
        memcpy_k_to_usr(uaddr, buf, addr_length);
    }

    OBOS_MAYBE_UNUSED fail1:
    Free(OBOS_KernelAllocator, buf, addr_length_user);

    return status;
}

obos_status Sys_PeerName(handle desc, sockaddr* uaddr, size_t addr_length, size_t* actual_addr_length)
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

    size_t const addr_length_user = addr_length;
    void* buf = Allocate(OBOS_KernelAllocator, addr_length, nullptr);
    sockaddr *addr = buf;
    status = memcpy_usr_to_k(addr, uaddr, addr_length);
    if (obos_is_error(status))
        goto fail1;

    status = Net_GetPeerName(fd->un.fd, addr, &addr_length);

    if (obos_is_success(status))
    {
        memcpy_k_to_usr(actual_addr_length, &addr_length, sizeof(addr_length));
        memcpy_k_to_usr(uaddr, buf, addr_length);
    }

    OBOS_MAYBE_UNUSED fail1:
    Free(OBOS_KernelAllocator, buf, addr_length_user);

    return status;
}

obos_status Sys_GetSockOpt(handle desc, int layer, int number, void *ubuffer, size_t *usize)
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

    size_t size = 0;
    status = memcpy_usr_to_k(&size, usize, sizeof(size_t));
    if (obos_is_error(status))
        return status;

    void* kbuf = Mm_MapViewOfUserMemory(
        CoreS_GetCPULocalPtr()->currentContext, 
        ubuffer, nullptr, size, 
        0, true, 
        &status);

    status = Net_GetSockOpt(fd->un.fd, layer, number, kbuf, &size);

    if (obos_is_success(status))
        memcpy_k_to_usr(usize, &size, sizeof(size));

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, size);

    return status;
}

obos_status Sys_SetSockOpt(handle desc, int layer, int number, const void *ubuffer, size_t size)
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

    void* kbuf = Mm_MapViewOfUserMemory(
        CoreS_GetCPULocalPtr()->currentContext, 
        (void*)ubuffer, nullptr, size, 
        OBOS_PROTECTION_READ_ONLY, true, 
        &status);

    status = Net_SetSockOpt(fd->un.fd, layer, number, kbuf, size);

    Mm_VirtualMemoryFree(&Mm_KernelContext, kbuf, size);

    return status;
}

obos_status Sys_ShutdownSocket(handle desc, int how)
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

    return Net_Shutdown(fd->un.fd, how);
}

obos_status Sys_FChmodAt(handle dirfd, const char* upathname, int mode, int flags)
{
    obos_status status = 0;

    status = OBOS_CapabilityCheck("fs/chmod", true);
    if (obos_is_error(status))
        return status;

    char* pathname = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upathname, nullptr, &sz_path);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, pathname, sz_path+1);
        return status;
    }
    pathname = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upathname, pathname, nullptr);

    vnode* target = nullptr;

    dirent* parent = nullptr;
    const char* name = nullptr;
    if (dirfd != AT_FDCWD)
    {
        if (HANDLE_TYPE(dirfd) != HANDLE_TYPE_DIRENT && HANDLE_TYPE(dirfd) != HANDLE_TYPE_FD)
        {
            status = OBOS_STATUS_INVALID_ARGUMENT;
            goto fail;
        }
        if (HANDLE_TYPE(dirfd) == HANDLE_TYPE_FD && strlen(pathname))
        {
            status = OBOS_STATUS_INVALID_ARGUMENT;
            goto fail;
        }
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, 0, true, &status);
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            goto fail;
        }
        if (HANDLE_TYPE(dirfd) == HANDLE_TYPE_FD)
        {
            target = desc->un.fd->vn;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            if (!target)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                goto fail;
            }
            goto have_target;
        }
        parent = desc->un.dirent->parent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

        if (strchr(pathname, '/') != strlen(pathname))
        {
            size_t last_slash = strrfind(pathname, '/');
            char ch = pathname[last_slash];
            pathname[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(pathname, *pathname == '/' ? Vfs_Root : parent);
            pathname[last_slash] = ch;
            name = pathname+last_slash+1;
        }
        else
            name = pathname;
    }
    else 
    {
        parent = Core_GetCurrentThread()->proc->cwd;
        if (strchr(pathname, '/') != strlen(pathname))
        {
            size_t last_slash = strrfind(pathname, '/');
            char ch = pathname[last_slash];
            pathname[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(pathname, *pathname == '/' ? Vfs_Root : parent);
            pathname[last_slash] = ch;
            name = pathname+last_slash+1;
        }
        else
            name = pathname;
    }

    dirent* targetd = VfsH_DirentLookupFrom(name, parent);
    if (!targetd)
    {
        status = OBOS_STATUS_NOT_FOUND;
        goto fail;
    }
    if (~flags & AT_SYMLINK_NOFOLLOW)
        targetd = VfsH_FollowLink(targetd);
    // NOTE(oberrow): AT_EMPTY_PATH is the default on obos, and can actually not be
    // changed, because of the nature of the lookup() function
    
    target = targetd->vnode;

    have_target:

    if (target->uid != Sys_GetUid() && Sys_GetUid() != ROOT_UID)
    {
        status = OBOS_STATUS_ACCESS_DENIED;
        goto fail;
    }

    file_perm real_mode = unix_to_obos_mode(mode, true);
    
    mount* mount = Vfs_GetVnodeMount(target);
    if (mount)
    {
        driver_header* header = Vfs_GetVnodeDriver(mount->root->vnode);
        if (!header)
        {
            status = OBOS_STATUS_INTERNAL_ERROR;
            goto fail;
        }
    
        status = !header->ftable.set_file_perms ? OBOS_STATUS_UNIMPLEMENTED : header->ftable.set_file_perms(target->desc, real_mode);
        if (obos_is_error(status))
            goto fail;
    }
    
    target->perm = real_mode;

    fail:
    Free(OBOS_KernelAllocator, pathname, sz_path+1);

    return status;
}

obos_status Sys_FChownAt(handle dirfd, const char *upathname, uid owner, gid group, int flags)
{
    obos_status status = OBOS_CapabilityCheck("fs/chown", false);
    if (obos_is_error(status))
        return status;

    char* pathname = nullptr;
    size_t sz_path = 0;
    status = OBOSH_ReadUserString(upathname, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    pathname = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upathname, pathname, nullptr);

    vnode* target = nullptr;

    dirent* parent = nullptr;
    const char* name = nullptr;
    if (dirfd != AT_FDCWD)
    {
        if (HANDLE_TYPE(dirfd) != HANDLE_TYPE_DIRENT && HANDLE_TYPE(dirfd) != HANDLE_TYPE_FD)
        {
            status = OBOS_STATUS_INVALID_ARGUMENT;
            goto fail;
        }
        if (HANDLE_TYPE(dirfd) == HANDLE_TYPE_FD && strlen(pathname))
        {
            status = OBOS_STATUS_INVALID_ARGUMENT;
            goto fail;
        }
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dirfd, 0, true, &status);
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            goto fail;
        }
        if (HANDLE_TYPE(dirfd) == HANDLE_TYPE_FD)
        {
            target = desc->un.fd->vn;
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            if (!target)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                goto fail;
            }
            goto have_target;
        }
        parent = desc->un.dirent->parent;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

        if (strchr(pathname, '/') != strlen(pathname))
        {
            size_t last_slash = strrfind(pathname, '/');
            char ch = pathname[last_slash];
            pathname[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(pathname, *pathname == '/' ? Vfs_Root : parent);
            pathname[last_slash] = ch;
            name = pathname+last_slash+1;
        }
        else
            name = pathname;
    }
    else 
    {
        parent = Core_GetCurrentThread()->proc->cwd;
        if (strchr(pathname, '/') != strlen(pathname))
        {
            size_t last_slash = strrfind(pathname, '/');
            char ch = pathname[last_slash];
            pathname[last_slash] = 0;
            parent = VfsH_DirentLookupFrom(pathname, *pathname == '/' ? Vfs_Root : parent);
            pathname[last_slash] = ch;
            name = pathname+last_slash+1;
        }
        else
            name = pathname;
    }

    dirent* targetd = VfsH_DirentLookupFrom(name, parent);
    if (!targetd)
    {
        status = OBOS_STATUS_NOT_FOUND;
        goto fail;
    }
    if (~flags & AT_SYMLINK_NOFOLLOW)
        targetd = VfsH_FollowLink(targetd);
    // NOTE(oberrow): AT_EMPTY_PATH is the default on obos, and can actually not be
    // changed, because of the nature of the lookup() function
    
    target = targetd->vnode;

    have_target:
    OBOS_ENSURE(target);

    driver_header* header = Vfs_GetVnodeDriver(target);
    if (!header)
    {
        status = OBOS_STATUS_INTERNAL_ERROR;
        goto fail;
    }

    status = !header->ftable.set_file_owner ? OBOS_STATUS_UNIMPLEMENTED : header->ftable.set_file_owner(target->desc, owner, group);
    if (obos_is_error(status))
        goto fail;

    if (owner != -1)
        target->uid = owner;
    if (group != -1)
        target->gid = group;

    fail:
    Free(OBOS_KernelAllocator, pathname, sz_path+1);

    return status;
}

void Sys_UMask(uint32_t mask, uint32_t* oldmask)
{
    if (oldmask)
        memcpy_k_to_usr(oldmask, &Core_GetCurrentThread()->proc->umask, sizeof(*oldmask));
    Core_GetCurrentThread()->proc->umask = mask;
}