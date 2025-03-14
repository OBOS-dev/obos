/*
 * oboskrnl/vfs/create.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>

#include <utils/string.h>

static bool has_write_perm(vnode* parent)
{
    uid current_uid = Core_GetCurrentThread()->proc->currentUID;
    uid current_gid = Core_GetCurrentThread()->proc->currentGID;
    if (parent->flags & VFLAGS_MOUNTPOINT)
    {
        drv_fs_info info = {};
        parent->un.mounted->fs_driver->driver->header.ftable.stat_fs_info(parent->un.mounted->device, &info);
        if (parent->owner_uid == current_uid || parent->group_uid == current_gid)
            return ~info.flags & FS_FLAGS_RDONLY;
        else
            return false; 
    }
    if (parent->owner_uid == current_uid)
        return parent->perm.owner_write;
    else if (parent->group_uid == current_gid)
        return parent->perm.group_write;
    else
        return parent->perm.other_write;
}

obos_status Vfs_CreateNode(dirent* parent, const char* name, uint32_t vtype, file_perm mode)
{
    if (!parent)
        parent = Vfs_Root;
    if (!name || !vtype || vtype >= VNODE_TYPE_BAD)
        return OBOS_STATUS_INVALID_ARGUMENT;
    vnode* parent_vn = parent->vnode;
    if (!parent_vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (parent_vn->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!has_write_perm(parent_vn))
        return OBOS_STATUS_ACCESS_DENIED;
    file_type type = 0;
    switch (vtype)
    {
        case VNODE_TYPE_REG:
            type = FILE_TYPE_REGULAR_FILE;
            break;
        case VNODE_TYPE_DIR:
            type = FILE_TYPE_DIRECTORY;
            break;
        case VNODE_TYPE_LNK:
            type = FILE_TYPE_SYMBOLIC_LINK;
            break;
        case VNODE_TYPE_BLK:
        case VNODE_TYPE_CHR:
        case VNODE_TYPE_SOCK:
        case VNODE_TYPE_FIFO:
        default:
            return OBOS_STATUS_INVALID_ARGUMENT;
    }
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    vn->group_uid = Core_GetCurrentThread()->proc->currentGID;
    vn->owner_uid = Core_GetCurrentThread()->proc->currentUID;
    vn->perm = mode;
    vn->flags = 0;
    vn->vtype = vtype;
    vn->refs++;
    vn->mount_point = parent_vn->mount_point;
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    OBOS_InitString(&ent->name, name);
    ent->vnode = vn;
    driver_ftable* ftable = &parent_vn->mount_point->fs_driver->driver->header.ftable;
    vnode* mount_vn = nullptr;
    if (parent_vn->flags & VFLAGS_MOUNTPOINT)
    {
        vn->mount_point = parent_vn->un.mounted;
        ftable = &parent_vn->un.mounted->fs_driver->driver->header.ftable;
        mount_vn = parent_vn->un.mounted->device;
    }
    obos_status status = ftable->mk_file(&vn->desc, parent_vn->flags & VFLAGS_MOUNTPOINT ? UINTPTR_MAX : parent_vn->desc, mount_vn, name, type);
    if (obos_is_error(status))
    {
        Vfs_Free(vn);
        Vfs_Free(ent);
        return status;
    }
    VfsH_DirentAppendChild(parent, ent);
    return status;
}