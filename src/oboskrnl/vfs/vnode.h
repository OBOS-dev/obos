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
};

// basically a struct specinfo, but renamed.
typedef struct vdev
{
    dev_desc desc;
    struct driver_id* driver;
    void* data;
    size_t refs;
} vdev;
typedef driver_file_perm file_perm;
typedef struct vnode
{
    void* data;
    uint32_t vtype;
    uint32_t flags;
    struct mount* mount_point;
    union {
        struct mount* mounted;
                vdev* device;
          const char* linked;
        // TODO: Add more stuff, such as pipes, sockets, etc.
    } un;
    size_t refs;
    file_perm perm;
    size_t filesize; // filesize.
    uid owner_uid; // the owner's UID.
    gid group_uid; // the group's GID.
    dev_desc desc; // the cached device descriptor.
    fd_list opened;
    struct partition* partitions;
    size_t nPartitions;
    uint32_t inode;

    size_t blkSize;
} vnode;
struct async_irp
{
    // This event object is set the operation is finished.
    event* e;
    union {
        const void* cbuf;
        void* buf;
    } un;
    size_t requestSize;
    thread* worker;
    bool rw : 1; // if false, the operation is a read, otherwise it is a write.
    bool cached : 1;
    uoff_t fileoff;
    vnode* vn;
};
OBOS_EXPORT vnode* Drv_AllocateVNode(driver_id* drv, dev_desc desc, size_t filesize, vdev** dev, uint32_t type);
