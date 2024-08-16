/*
 * oboskrnl/vfs/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <cmdline.h>

#include <vfs/init.h>
#include <vfs/dirent.h>
#include <vfs/mount.h>
#include <vfs/alloc.h>
#include <vfs/vnode.h>
#include <vfs/fd.h>

#include <utils/string.h>

#include <allocators/base.h>

#include <driver_interface/driverId.h>

#include <uacpi_libc.h>

// InitRD driver name.
#include <generic/initrd/name.h>

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
    Vfs_Root->vnode->perm.group_exec = false;
    Vfs_Root->vnode->perm.group_write = true;
    Vfs_Root->vnode->perm.group_read = true;
    Vfs_Root->vnode->perm.owner_exec = false;
    Vfs_Root->vnode->perm.owner_write = true;
    Vfs_Root->vnode->perm.owner_read = true;
    Vfs_Root->vnode->perm.other_exec = false;
    Vfs_Root->vnode->perm.other_write = false;
    Vfs_Root->vnode->perm.other_read = true;
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
        return;
    mount* root = nullptr;
    Vfs_Mount("/", nullptr, &initrd_dev, &root);
    Vfs_Root->vnode->mount_point = root;
    const char* const pathspec = "/test_folder/file.txt";
    fd file = {};
    obos_status status = Vfs_FdOpen(&file, pathspec, FD_OFLAGS_READ_ONLY);
    if (obos_is_error(status))
    {
        OBOS_Debug("Could not open %s. Status: %s\n", pathspec, status);
        goto end;
    }
    char buf[16];
    Vfs_FdRead(&file, buf, 13, nullptr);
    OBOS_Debug("%s:\n", pathspec);
    printf("%s\n", buf);
    end:
    if (root_partid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_partid, strlen(root_partid));
    if (root_uuid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_uuid, strlen(root_uuid));
}