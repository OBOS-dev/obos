/*
 * oboskrnl/vfs/vnode.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/fd.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <utils/list.h>

#include <mm/page.h>

#include <locks/mutex.h>

enum
{
    // This vnode has no type.
    VNODE_TYPE_NON,
    // This vnode represents a regular file.
    VNODE_TYPE_REG,
    // This vnode represents a directory.
    VNODE_TYPE_DIR,
    // This vnode represents a block device.
    VNODE_TYPE_BLK,
    // This vnode represents a character device.
    VNODE_TYPE_CHR,
    // This vnode represents a symbolic link.
    VNODE_TYPE_LNK,
    // This vnode represents a socket.
    VNODE_TYPE_SOCK,
    // This vnode represents a named pipe.
    VNODE_TYPE_FIFO,
    // This vnode represents a bad or dead file.
    VNODE_TYPE_BAD
};
enum 
{
    VFLAGS_MOUNTPOINT = 1,
    VFLAGS_IS_TTY = 2,
    VFLAGS_PARTITION = 4,
    VFLAGS_FB = 8,
    // A file that only provides events, and cannot be read/written.
    VFLAGS_EVENT_DEV = 16,
    // The driver implementing this vnode is DEAD and should NOT be used.
    VFLAGS_DRIVER_DEAD = 32,
    VFLAGS_NIC_NO_FCS = 64,
};

// basically a struct specinfo, but renamed.
typedef struct vdev
{
    dev_desc desc;
    struct driver_id* driver;
    void* data;
    size_t refs;
} vdev;

// All these times are since the Unix Epoch (January 1st, 1970)
struct file_times
{
    long access;
    long change;
    long birth;
};

typedef driver_file_perm file_perm;
typedef struct vnode
{
    union {
        void* data;
        struct net_tables* net_tables;
    };
    uint32_t vtype;
    uint32_t flags;
    struct mount* mount_point;
    union {
        struct mount* mounted;
                vdev* device;
          const char* linked;
               event* evnt;
        // TODO: Add more stuff, such as pipes, sockets, etc.
    } un;
    size_t refs;
    file_perm perm;
    size_t filesize; // filesize.
    uid uid; // the owner's UID.
    gid gid; // the group's GID.
    dev_desc desc; // the cached device descriptor.
    fd_list opened;
    size_t nMappedRegions;
    size_t nWriteableMappedRegions;
    struct partition* partitions;
    size_t nPartitions;
    uint32_t inode;

    struct file_times times;

    size_t blkSize;

#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
    int seals;

    pagecache_tree cache;
} vnode;

OBOS_EXPORT vnode* Drv_AllocateVNode(driver_id* drv, dev_desc desc, size_t filesize, vdev** dev, uint32_t type);

// For files that can have I/O on them (FIFOs, regular files, CHR/BLK devices, and sockets)
OBOS_EXPORT driver_header* Vfs_GetVnodeDriver(vnode* vn);
// For files that can and can't have I/O on them (directories, symbolic links, FIFOs, regular files, CHR/BLK devices, and sockets)
OBOS_EXPORT driver_header* Vfs_GetVnodeDriverStat(vnode* vn);
OBOS_EXPORT struct mount* Vfs_GetVnodeMount(vnode* vn);

OBOS_EXPORT obos_status Vfs_Access(uid asUid, gid asGid, vnode* vn, bool read, bool write, bool exec);