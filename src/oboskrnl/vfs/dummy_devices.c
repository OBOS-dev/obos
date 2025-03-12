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

OBOS_EXPORT const char* OBOS_ScancodeToString[84] = {
    "SCANCODE_UNKNOWN",
    "SCANCODE_A",
    "SCANCODE_B",
    "SCANCODE_C",
    "SCANCODE_D",
    "SCANCODE_E",
    "SCANCODE_F",
    "SCANCODE_G",
    "SCANCODE_H",
    "SCANCODE_I",
    "SCANCODE_J",
    "SCANCODE_K",
    "SCANCODE_L",
    "SCANCODE_M",
    "SCANCODE_N",
    "SCANCODE_O",
    "SCANCODE_P",
    "SCANCODE_Q",
    "SCANCODE_R",
    "SCANCODE_S",
    "SCANCODE_T",
    "SCANCODE_U",
    "SCANCODE_V",
    "SCANCODE_W",
    "SCANCODE_X",
    "SCANCODE_Y",
    "SCANCODE_Z",
    "SCANCODE_0",
    "SCANCODE_1",
    "SCANCODE_2",
    "SCANCODE_3",
    "SCANCODE_4",
    "SCANCODE_5",
    "SCANCODE_6",
    "SCANCODE_7",
    "SCANCODE_8",
    "SCANCODE_9",
    "SCANCODE_PLUS",
    "SCANCODE_FORWARD_SLASH",
    "SCANCODE_BACKSLASH",
    "SCANCODE_STAR",
    "SCANCODE_EQUAL",
    "SCANCODE_DASH",
    "SCANCODE_UNDERSCORE",
    "SCANCODE_BACKTICK",
    "SCANCODE_QUOTATION_MARK",
    "SCANCODE_APOSTROPHE",
    "SCANCODE_SQUARE_BRACKET_LEFT",
    "SCANCODE_SQUARE_BRACKET_RIGHT",
    "SCANCODE_TAB",
    "SCANCODE_ESC",
    "SCANCODE_PGUP",
    "SCANCODE_PGDOWN",
    "SCANCODE_HOME",
    "SCANCODE_END",
    "SCANCODE_DELETE",
    "SCANCODE_BACKSPACE",
    "SCANCODE_SPACE",
    "SCANCODE_INSERT",
    "SCANCODE_F1",
    "SCANCODE_F2",
    "SCANCODE_F3",
    "SCANCODE_F4",
    "SCANCODE_F5",
    "SCANCODE_F6",
    "SCANCODE_F7",
    "SCANCODE_F8",
    "SCANCODE_F9",
    "SCANCODE_F10",
    "SCANCODE_F11",
    "SCANCODE_F12",
    "SCANCODE_DOT",
    "SCANCODE_COMMA",
    "SCANCODE_SEMICOLON",
    "SCANCODE_UP_ARROW",
    "SCANCODE_DOWN_ARROW",
    "SCANCODE_RIGHT_ARROW",
    "SCANCODE_LEFT_ARROW",
    "SCANCODE_ENTER",
    "SCANCODE_SUPER_KEY",
    "SCANCODE_CTRL",
    "SCANCODE_ALT",
    "SCANCODE_FN",
    "SCANCODE_SHIFT",
};