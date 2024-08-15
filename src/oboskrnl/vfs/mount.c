/*
 * oboskrnl/vfs/mount.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>

#include <utils/list.h>

#include <vfs/namecache.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <locks/mutex.h>

struct dirent* Vfs_Root;

obos_status Vfs_Mount(const char* at, vdev* device, mount** mountpoint)
{
    if (!Vfs_Root)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    OBOS_UNUSED(at);
    OBOS_UNUSED(device);
    OBOS_UNUSED(mountpoint);
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_Unmount(mount* what);
obos_status Vfs_UnmountP(const char* at);