/*
 * oboskrnl/vfs/mount.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <utils/list.h>

#include <vfs/namecache.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>

#include <locks/mutex.h>

typedef LIST_HEAD(mount_list, struct mount) mount_list;
LIST_PROTOTYPE(mount_list, struct mount, node);
typedef struct mount
{
    LIST_NODE(mount_list, struct mount) node;
    mutex lock;
    dirent* root;
    vdev* fs_driver;
    vnode* device; // the block device the filesystem is situated on.
    vnode* mounted_on;
    namecache nc;
    dirent_list dirent_list;
    atomic_size_t nWaiting;
    bool awaitingFree;
} mount;
extern struct dirent* Vfs_Root;
extern struct dirent* Vfs_DevRoot;
extern mount_list Vfs_Mounted;

// returns true if the operation succeeded.
bool VfsH_LockMountpoint(mount* point);
// returns true if the operation succeeded.
bool VfsH_UnlockMountpoint(mount* point);

obos_status Vfs_Mount(const char* at, vnode* on, vdev* fs_driver, mount** mountpoint);
obos_status Vfs_Unmount(mount* what);
obos_status Vfs_UnmountP(const char* at);

obos_status Vfs_StatFSInfo(struct vnode* vn, struct drv_fs_info* out);
