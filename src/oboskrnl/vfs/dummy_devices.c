/*
 * oboskrnl/vfs/dummy_devices.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>

#include <driver_interface/header.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>

enum {
    // /dev/null
    DUMMY_NULL = 1,
    // /dev/full
    DUMMY_FULL,
    // /dev/zero
    DUMMY_ZERO,
    DUMMY_MAX = DUMMY_ZERO,
};
static const char* names[DUMMY_MAX+1] = {
    nullptr,
    "null", "full", "zero"
};

static obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    if (!blkSize || desc < 1 || desc > DUMMY_MAX)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    if (!count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
        case DUMMY_FULL:
        case DUMMY_ZERO:
            *count = 0;
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    if (!buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
            *nBlkRead = 0;
            break;
        case DUMMY_FULL:
        case DUMMY_ZERO:
            memzero(buf, blkCount);
            if (nBlkRead)
                *nBlkRead = blkCount;
            break;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    if (!buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    switch (desc) {
        case DUMMY_NULL:
        case DUMMY_ZERO:
            *nBlkWritten = blkCount;
            break;
        case DUMMY_FULL:
            return OBOS_STATUS_NOT_ENOUGH_MEMORY;
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static void driver_cleanup_callback(){};
static obos_status ioctl(dev_desc what, uint32_t request, void* argp) { OBOS_UNUSED(what); OBOS_UNUSED(request); OBOS_UNUSED(argp); return OBOS_STATUS_INVALID_IOCTL; }

driver_id OBOS_DummyDriver = {
    .id=0,
    .header = {
        .magic = OBOS_DRIVER_MAGIC,
        .flags = DRIVER_HEADER_FLAGS_NO_ENTRY|DRIVER_HEADER_HAS_VERSION_FIELD,
        .ftable = {
            .get_blk_size = get_blk_size,
            .get_max_blk_count = get_max_blk_count,
            .write_sync = write_sync,
            .read_sync = read_sync,
            .ioctl = ioctl,
            .driver_cleanup_callback = driver_cleanup_callback,
        },
        .driverName = "Dummy Device Driver"
    }
};
vdev OBOS_DummyDriverVdev = {
    .driver = &OBOS_DummyDriver,
};

static void init_desc(dev_desc desc)
{
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));

    vn->owner_uid = 0;
    vn->group_uid = 0;
    vn->desc = desc;

    vn->perm.owner_exec=false;
    vn->perm.group_exec=false;
    vn->perm.other_exec=false;

    vn->perm.owner_read=true;
    vn->perm.group_read=true;
    vn->perm.other_read=true;

    vn->perm.owner_write=true;
    vn->perm.group_write=true;
    vn->perm.other_write=true;

    vn->vtype = VNODE_TYPE_CHR;
    vn->un.device = &OBOS_DummyDriverVdev;
    ent->vnode = vn;
    vn->refs++;
    OBOS_InitString(&ent->name, names[vn->desc]);

    dirent* parent = VfsH_DirentLookup(OBOS_DEV_PREFIX);
    if (!parent)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Could not find directory at OBOS_DEV_PREFIX (%s) specified at build time.\n", __func__, OBOS_DEV_PREFIX);
    vn->mount_point = parent->vnode->mount_point;
    VfsH_DirentAppendChild(parent, ent);
}

void Vfs_InitDummyDevices()
{
    init_desc(DUMMY_NULL);
    init_desc(DUMMY_FULL);
    init_desc(DUMMY_ZERO);
}
