/*
 * oboskrnl/vfs/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "driver_interface/header.h"
#include "driver_interface/loader.h"
#include "mm/alloc.h"
#include "mm/context.h"
#include "scheduler/thread.h"

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

#include <locks/event.h>
#include <locks/wait.h>

/*
    "--mount-initrd=pathspec: Mounts the InitRD at pathspec if specified, otherwise the initrd is left unmounted."
    "--root-fs-uuid=uuid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
    "                     is used as root."
    "--root-fs-partid=partid: Specifies the partition to mount as root. If set to 'initrd', the initrd"
    "                         is used as root."
*/

static iterate_decision foreach_dev(dev_desc desc, size_t unused1, size_t unused2, void* udata)
{
    OBOS_UNUSED(unused1);
    OBOS_UNUSED(unused2);
    driver_id* drv = (driver_id*)udata;
    const char* dev_name = nullptr;
    drv->header.ftable.query_user_readable_name(desc, &dev_name);
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    vdev* vdrv = Vfs_Calloc(1, sizeof(vdev));
    vdrv->driver = drv;
    vdrv->data = nullptr;
    vdrv->desc = desc;
    vn->desc = desc;
    vn->un.device = vdrv;
    vn->vtype = VNODE_TYPE_CHR;
    vn->filesize = 0;
    vn->group_uid = ROOT_GID;
    vn->owner_uid = ROOT_UID;
    vn->perm.owner_read = true;
    vn->perm.group_read = true;
    vn->perm.owner_write = true;
    vn->perm.group_write = true;
    dirent* slash_dev = VfsH_DirentLookup("/dev");
    vn->mount_point = slash_dev->vnode->mount_point;
    dirent* ent = Vfs_Calloc(1, sizeof(dirent));
    ent->vnode = vn;
    OBOS_InitString(&ent->name, dev_name);
    VfsH_DirentAppendChild(slash_dev, ent);
    return ITERATE_DECISION_CONTINUE;
}
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
    const char* pathspec = "/uart";
    fd file = {};
    obos_status status = Vfs_FdOpen(&file, pathspec, FD_OFLAGS_READ_ONLY);
    if (obos_is_error(status))
    {
        OBOS_Debug("Could not open %s. Status: %d\n", pathspec, status);
        goto end;
    }
    Vfs_FdSeek(&file, 0, SEEK_END);
    size_t filesize = Vfs_FdTellOff(&file);
    Vfs_FdSeek(&file, 0, SEEK_SET);
    void* buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, filesize, OBOS_PROTECTION_READ_ONLY, VMA_FLAGS_PRIVATE, &file, &status);
    if (obos_is_error(status))
    {
        OBOS_Debug("Could not map view of %s. Status: %d\n", pathspec, status);
        goto end;
    }
    driver_id* driver = Drv_LoadDriver(buf, filesize, &status);
    if (obos_is_error(status))
    {
        OBOS_Debug("Could not load %s. Status: %d\n", pathspec, status);
        goto end;
    }
    thread* mainThr = nullptr;
    Drv_StartDriver(driver, &mainThr);
    // Mm_VirtualMemoryFree(&Mm_KernelContext, buf, filesize);
    while (!(mainThr->flags & THREAD_FLAGS_DIED))
        ;
    driver->header.ftable.foreach_device(foreach_dev, driver);
    Vfs_FdClose(&file);
    pathspec = "/dev/COM1";
    Vfs_FdOpen(&file, pathspec, FD_OFLAGS_UNCACHED);
    Vfs_FdIoctl(&file, 6, /* IOCTL_OPEN_SERIAL_CONNECTION */0, 
        1,
        115200,
        /* EIGHT_DATABITS */ 3,
        /* ONE_STOPBIT */ 0,
        /* PARITYBIT_NONE */ 0,
        &file.vn->desc);
    file.vn->un.device->desc = file.vn->desc;
    char message[15] = {};
    Vfs_FdRead(&file, message, 14, nullptr);
    Vfs_FdWrite(&file, message, 14, nullptr);
    Vfs_FdClose(&file);
    end:
    if (root_partid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_partid, strlen(root_partid));
    if (root_uuid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_uuid, strlen(root_uuid));
}