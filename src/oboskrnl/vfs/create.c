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
#include <vfs/socket.h>
#include <vfs/create.h>

#include <utils/string.h>
#include <utils/list.h>

#ifdef __x86_64__
#   include <arch/x86_64/cmos.h>
#endif

obos_status Vfs_CreateNode(dirent* parent, const char* name, uint32_t vtype, file_perm mode)
{
    if (!parent)
        parent = Vfs_Root;
    if (!name || !vtype || vtype >= VNODE_TYPE_BAD)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = Vfs_Access(Core_GetCurrentThread()->proc->euid, 
                                    Core_GetCurrentThread()->proc->egid,
                                    parent->vnode, 
                                    false,
                                    true,
                                    false);
    if (obos_is_error(status))
        return status;

    vnode* parent_vn = parent->vnode;
    if (!parent_vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (parent_vn->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
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
    vn->gid = Core_GetCurrentThread()->proc->egid;
    vn->uid = Core_GetCurrentThread()->proc->euid;
    vn->perm = mode;
    vn->flags = 0;
    vn->vtype = vtype;
    vn->mount_point = parent_mnt;
    
    long current_time = 0;
#ifdef __x86_64__
    Arch_CMOSGetEpochTime(&current_time);
#endif

    vn->times.access = current_time;
    vn->times.birth = current_time;
    vn->times.change = current_time;

    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    OBOS_InitString(&ent->name, name);
    ent->vnode = vn;
    vnode* mount_vn = parent_mnt->device;
    status = OBOS_STATUS_SUCCESS;
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
    Vfs_UpdateFileTime(vn);
    if (ftable->get_file_inode)
        ftable->get_file_inode(vn->desc, &vn->inode);
    VfsH_DirentAppendChild(parent, ent);
    LIST_APPEND(dirent_list, &parent_mnt->dirent_list, ent);
    ent->vnode->refs++;
    return status;
}

OBOS_EXPORT obos_status Vfs_UnlinkNode(dirent* node)
{
    if (!node)
        return OBOS_STATUS_SUCCESS;
    if (node->d_children.nChildren)
        return OBOS_STATUS_IN_USE; // cannot remove a directory with children
    obos_status status = Vfs_Access(Core_GetCurrentThread()->proc->euid, 
                                    Core_GetCurrentThread()->proc->egid,
                                    node->d_parent->vnode, 
                                    false,
                                    true,
                                    false);
    if (obos_is_error(status))
        return status;

    mount *parent_mnt = node->vnode->flags & VFLAGS_MOUNTPOINT ? 
        node->vnode->un.mounted:
        node->vnode->mount_point;
    driver_ftable* ftable = &parent_mnt->fs_driver->driver->header.ftable;
    if (!ftable->remove_file)
        return OBOS_STATUS_UNIMPLEMENTED;
    if (LIST_GET_NODE_COUNT(fd_list, &node->vnode->opened))
        return OBOS_STATUS_IN_USE; // TODO: Handle correctly (when the last FD is closed, then free+delete the vnode)

    status = OBOS_STATUS_SUCCESS;

    if (node->vnode->vtype == VNODE_TYPE_DIR || node->vnode->vtype == VNODE_TYPE_REG || node->vnode->vtype == VNODE_TYPE_LNK)
    {
        if (parent_mnt->fs_driver->driver->header.flags & DRIVER_HEADER_DIRENT_CB_PATHS)
        {
            char* path = VfsH_DirentPath(node, parent_mnt->root);
            status = ftable->premove_file(parent_mnt->device, path);
            Vfs_Free(path);   
        }
        else 
            status = ftable->remove_file(node->vnode->desc);
    }

    if (obos_is_error(status))
        return status;

    // Remove it in the VFS structures

    VfsH_DirentRemoveChild(node->d_parent, node);
    OBOS_FreeString(&node->name);
    LIST_REMOVE(dirent_list, &parent_mnt->dirent_list, node);
    --node->vnode->refs; // Removed from dirent list.
    if (!node->vnode->refs)
    {
        for (size_t i = 0; i < node->vnode->nPartitions; i++)
            LIST_REMOVE(partition_list, &OBOS_Partitions, &node->vnode->partitions[i]);
        Vfs_Free(node->vnode->partitions);

        if (node->vnode->vtype == VNODE_TYPE_SOCK)
        {
            struct socket_desc* desc = (void*)node->vnode->desc;
            desc->ops->free(desc);
            Vfs_Free(desc);
        }

        Vfs_Free(node->vnode);
    }
    Vfs_Free(node);

    return OBOS_STATUS_SUCCESS;
}

obos_status Vfs_RenameNode(dirent* node, dirent* newparent, const char* name)
{
    if (!node)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    // No-op
    if (!newparent && !name)
        return OBOS_STATUS_SUCCESS;

    driver_header* header = Vfs_GetVnodeDriver(node->vnode);
    if (!header)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = Vfs_Access(Core_GetCurrentThread()->proc->euid, 
                                Core_GetCurrentThread()->proc->egid,
                                (newparent ? newparent : node->d_parent)->vnode, 
                                false,
                                true,
                                false);
    if (obos_is_error(status))
        return status;

    // Rename the entry
    if (!newparent || newparent == node->d_parent)
    {
        if (!header->ftable.move_desc_to)
            return OBOS_STATUS_UNIMPLEMENTED;
        if (!name)
            return OBOS_STATUS_INVALID_ARGUMENT;

        obos_status status = OBOS_STATUS_SUCCESS;
        if (header->flags & DRIVER_HEADER_DIRENT_CB_PATHS)
        {
            char* node_path = VfsH_DirentPath(node, Vfs_Root);
            status = header->ftable.pmove_desc_to(node->vnode, node_path, nullptr, name);
            Vfs_Free(node_path);
        }
        else
            status = header->ftable.move_desc_to(node->vnode->desc, 0, name);

        if (obos_is_success(status))
        {
            OBOS_FreeString(&node->name);
            OBOS_InitString(&node->name, name);
        }

        return status;
    }
    // Move the entry without renaming it
    if (newparent && !name)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        if (header->flags & DRIVER_HEADER_DIRENT_CB_PATHS)
        {
            char* node_path = VfsH_DirentPath(node, Vfs_Root);
            char* parent_path = VfsH_DirentPath(newparent, Vfs_Root);
            status = header->ftable.pmove_desc_to(node->vnode, node_path, parent_path, nullptr);
            Vfs_Free(parent_path);
            Vfs_Free(node_path);
        }
        else
            status = header->ftable.move_desc_to(node->vnode->desc, newparent->vnode->desc, nullptr);

        if (obos_is_success(status))
        {
            // if we don't do this, the vnode might be deleted when
            // the dirent is removed
            node->vnode->refs++;
            VfsH_DirentRemoveChild(node->d_parent, node);
            VfsH_DirentAppendChild(newparent, node);
            node->vnode->refs--;
        }

        return status;
    }

    // Move and rename the entry.

    status = OBOS_STATUS_SUCCESS;
    if (header->flags & DRIVER_HEADER_DIRENT_CB_PATHS)
    {
        char* node_path = VfsH_DirentPath(node, Vfs_Root);
        char* parent_path = VfsH_DirentPath(newparent, Vfs_Root);
        status = header->ftable.pmove_desc_to(node->vnode, node_path, parent_path, name);
        Vfs_Free(parent_path);
        Vfs_Free(node_path);
    }
    else
        status = header->ftable.move_desc_to(node->vnode->desc, newparent->vnode->desc, name);

    if (obos_is_success(status))
    {
        // if we don't do this, the vnode might be deleted when
        // the dirent is removed
        node->vnode->refs++;
        VfsH_DirentRemoveChild(node->d_parent, node);
        VfsH_DirentAppendChild(newparent, node);
        node->vnode->refs--;
        OBOS_FreeString(&node->name);
        OBOS_InitString(&node->name, name);
    }

    return status;
}

obos_status Vfs_UpdateFileTime(vnode* vn)
{
    if (!vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    driver_header* header = Vfs_GetVnodeDriver(vn);
    if (!header)
        return OBOS_STATUS_SUCCESS;
    if (!header->ftable.set_file_times)
        return OBOS_STATUS_SUCCESS;
    obos_status status = header->ftable.set_file_times(vn->desc, &vn->times);
    if (status == OBOS_STATUS_UNIMPLEMENTED) return OBOS_STATUS_SUCCESS;
    return status;
}