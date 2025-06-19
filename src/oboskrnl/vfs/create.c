/*
 * oboskrnl/vfs/create.c
 *
 * Copyright (c) 2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <partition.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>

#include <utils/string.h>
#include <utils/list.h>

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
    mount *parent_mnt = parent_vn->flags & VFLAGS_MOUNTPOINT ? 
        parent_vn->un.mounted:
        parent_vn->mount_point;
    driver_ftable* ftable = &parent_mnt->fs_driver->driver->header.ftable;
    if (!ftable->mk_file)
        return OBOS_STATUS_UNIMPLEMENTED;

    do {
        dirent* found = VfsH_DirentLookupFrom(name, parent);
        if (found)
            return OBOS_STATUS_ALREADY_INITIALIZED;
    } while(0);

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
    vn->mount_point = parent_mnt;
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    OBOS_InitString(&ent->name, name);
    ent->vnode = vn;
    vnode* mount_vn = parent_mnt->device;
    obos_status status = OBOS_STATUS_SUCCESS;
    if (parent_mnt->fs_driver->driver->header.flags & DRIVER_HEADER_DIRENT_CB_PATHS)
    {
        char* parent_path = VfsH_DirentPath(parent, parent_mnt->root);
        status = ftable->pmk_file(&vn->desc, parent_path, mount_vn, name, type, mode);
        Vfs_Free(parent_path);
    }
    else
        status = ftable->mk_file(&vn->desc, parent_vn->flags & VFLAGS_MOUNTPOINT ? UINTPTR_MAX : parent_vn->desc, mount_vn, name, type, mode);
    if (obos_is_error(status))
    {
        Vfs_Free(vn);
        Vfs_Free(ent);
        return status;
    }
    VfsH_DirentAppendChild(parent, ent);
    LIST_APPEND(dirent_list, &parent_mnt->dirent_list, ent);
    ent->vnode->refs++;
    return status;
}

obos_status Vfs_UnlinkNode(dirent* node)
{
    if (!node)
        return OBOS_STATUS_SUCCESS;
    if (node->d_children.nChildren)
        return OBOS_STATUS_IN_USE; // cannot remove a directory with children
    if (!has_write_perm(node->d_parent->vnode))
        return OBOS_STATUS_ACCESS_DENIED;

    mount *parent_mnt = node->vnode->flags & VFLAGS_MOUNTPOINT ? 
        node->vnode->un.mounted:
        node->vnode->mount_point;
    driver_ftable* ftable = &parent_mnt->fs_driver->driver->header.ftable;
    if (!ftable->remove_file)
        return OBOS_STATUS_UNIMPLEMENTED;
    if (LIST_GET_NODE_COUNT(fd_list, &node->vnode->opened))
        return OBOS_STATUS_IN_USE; // TODO: Handle correctly (when the last FD is closed, then free+delete the vnode)

    obos_status status = OBOS_STATUS_SUCCESS;

    if (parent_mnt->fs_driver->driver->header.flags & DRIVER_HEADER_DIRENT_CB_PATHS)
    {
        char* path = VfsH_DirentPath(node, parent_mnt->root);
        status = ftable->premove_file(parent_mnt->device, path);
        Vfs_Free(path);   
    }
    else 
        status = ftable->remove_file(node->vnode->desc);

    if (obos_is_error(status))
        return status;

    // Remove it in the VFS structures

    VfsH_DirentRemoveChild(node->d_parent, node);
    OBOS_FreeString(&node->name);
    LIST_REMOVE(dirent_list, &parent_mnt->dirent_list, node);
    --node->vnode->refs; // Removed from dirent list.
    if (!(--node->vnode->refs))
    {
        for (size_t i = 0; i < node->vnode->nPartitions; i++)
            LIST_REMOVE(partition_list, &OBOS_Partitions, &node->vnode->partitions[i]);
        Vfs_Free(node->vnode->partitions);

        Vfs_Free(node->vnode);
    }
    Vfs_Free(node);

    return OBOS_STATUS_SUCCESS;
}