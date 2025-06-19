/*
 * oboskrnl/vfs/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <partition.h>
#include <cmdline.h>

#include <vfs/init.h>
#include <vfs/dirent.h>
#include <vfs/mount.h>
#include <vfs/alloc.h>
#include <vfs/vnode.h>
#include <vfs/fd.h>
#include <vfs/pipe.h>
#include <vfs/create.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <utils/string.h>
#include <utils/uuid.h>

#include <allocators/base.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>
#include <driver_interface/loader.h>

#include <scheduler/thread.h>

#include <uacpi_libc.h>

// InitRD driver name.
#include <generic/initrd/name.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <utils/list.h>

/*
    "--mount-initrd=pathspec: Mounts the InitRD at pathspec if specified, otherwise the initrd is left unmounted."
    "--root-fs-uuid=uuid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
    "                     is used as root."
    "--root-fs-partid=partid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
    "                         is used as root."
*/

void Vfs_Initialize()
{
    char* root_uuid = OBOS_GetOPTS("root-fs-uuid");
    char* root_partid = OBOS_GetOPTS("root-fs-partid");
    if (!root_uuid && !root_partid)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Neither a root UUID, nor a root PARTID was specified.\n");
    if (root_uuid && root_partid)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Options, 'root-fs-uuid' and 'root-fs-partid', are mutually exclusive.\n");
    Vfs_Root = Vfs_Calloc(1, sizeof(dirent));
    OBOS_StringSetAllocator(&Vfs_Root->name, Vfs_Allocator);
    OBOS_InitString(&Vfs_Root->name, "/");
    Vfs_Root->vnode = Vfs_Calloc(1, sizeof(vnode));
    Vfs_Root->vnode->vtype = VNODE_TYPE_DIR;
    Vfs_Root->vnode->perm.group_exec = true;
    Vfs_Root->vnode->perm.group_write = true;
    Vfs_Root->vnode->perm.group_read = true;
    Vfs_Root->vnode->perm.owner_exec = true;
    Vfs_Root->vnode->perm.owner_write = true;
    Vfs_Root->vnode->perm.owner_read = true;
    Vfs_Root->vnode->perm.other_exec = true;
    Vfs_Root->vnode->perm.other_write = false;
    Vfs_Root->vnode->perm.other_read = true;
    Vfs_Root->vnode->desc = UINTPTR_MAX;
    vdev initrd_dev = { };
    for (driver_node* cur = Drv_LoadedDrivers.head; cur; )
    {
        if (uacpi_strncmp(cur->data->header.driverName, INITRD_DRIVER_NAME, 32) == 0)
        {
            initrd_dev.driver = cur->data;
            break;
        }
        cur = cur->next;
    }
    if (!initrd_dev.driver)
    {
        // We need to create OBOS_DEV_PREFIX
        file_perm perm = {};
        perm.owner_exec = true;
        perm.owner_read = true;
        perm.owner_write = true;
        perm.group_exec = true;
        perm.group_read = true;
        perm.group_write = false;
        perm.other_exec = true;
        perm.other_read = true;
        perm.other_write = false;
        Vfs_DevRoot = Vfs_Calloc(1, sizeof(dirent));
        OBOS_InitString(&Vfs_DevRoot->name, "dev");
        Vfs_DevRoot->vnode = Vfs_Calloc(1, sizeof(vnode));
        Vfs_DevRoot->vnode->blkSize = 1;
        Vfs_DevRoot->vnode->vtype = VNODE_TYPE_DIR;
        Vfs_DevRoot->vnode->perm = perm;
        return;
    }
    Vfs_Mount("/", nullptr, &initrd_dev, &Vfs_Root->vnode->mount_point);
    Vfs_DevRoot = VfsH_DirentLookup(OBOS_DEV_PREFIX);
    if (!Vfs_DevRoot)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Could not find directory at OBOS_DEV_PREFIX (%s) specified at build time.\n", __func__, OBOS_DEV_PREFIX);
    if (root_partid)
        Free(OBOS_KernelAllocator, root_partid, strlen(root_partid));
    if (root_uuid)
        Free(OBOS_KernelAllocator, root_uuid, strlen(root_uuid));
    Vfs_InitializePipeInterface();
}
OBOS_PAGEABLE_FUNCTION void Vfs_FinalizeInitialization()
{
    char* root_uuid_str = OBOS_GetOPTS("root-fs-uuid");
    char* root_partid = OBOS_GetOPTS("root-fs-partid");
    if (strcmp(root_partid ? root_partid : root_uuid_str, "initrd"))
        goto end; // We needn't do anything.
    uuid root_uuid = {};
    if (root_uuid_str)
    {
        string str = {};
        OBOS_InitString(&str, root_uuid_str);
        OBOS_UUIDToString(&root_uuid, &str);
    }
    partition* to_mount = nullptr;
    for (partition* part = LIST_GET_HEAD(partition_list, &OBOS_Partitions); part && !to_mount; )
    {
        if (root_uuid_str)
        {
            if (part->format != PARTITION_FORMAT_GPT)
                goto down;
            if (memcmp(root_uuid, part->part_uuid, sizeof(uuid)))
                to_mount = part;
        }   
        else
        {
            if (OBOS_CompareStringC(&part->partid, root_partid))
                to_mount = part;
        }

        down:
        part = LIST_GET_NEXT(partition_list, &OBOS_Partitions, part);
    }
    if (!to_mount)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find partition %s\n", root_uuid_str ? root_uuid_str : root_partid);
    if (!to_mount->fs_driver) 
       OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No filesystem driver exists for partition %s\n", root_uuid_str ? root_uuid_str : root_partid);
    Vfs_UnmountP("/");
    memzero(&Vfs_Root->tree_info, sizeof(Vfs_Root->tree_info));
    Vfs_DevRoot->tree_info.next_child = nullptr;
    Vfs_DevRoot->tree_info.prev_child = nullptr;
    vdev fs_vdev = {.driver=to_mount->fs_driver};
    Vfs_Mount("/", to_mount->vn, &fs_vdev, &Vfs_Root->vnode->mount_point);
    dirent* dev = VfsH_DirentLookup(OBOS_DEV_PREFIX);
    if (!dev)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Could not find directory at OBOS_DEV_PREFIX (%s) specified at build time.\n", __func__, OBOS_DEV_PREFIX);
    dirent* parent = dev ? dev->d_parent : nullptr;
    if (dev)
        VfsH_DirentRemoveChild(parent, dev);
    Vfs_DevRoot->vnode = dev->vnode;
    VfsH_DirentAppendChild(parent, Vfs_DevRoot);
    end:
    if (root_partid)
        Free(OBOS_KernelAllocator, root_partid, strlen(root_partid));
    if (root_uuid_str)
        Free(OBOS_KernelAllocator, root_uuid_str, strlen(root_uuid_str));
    Vfs_InitDummyDevices();
}
