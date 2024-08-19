/*
 * oboskrnl/vfs/init.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

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

static void test_thread(void* userdata)
{
    uintptr_t *udata = userdata;
    event* evnt = (event*)udata[0];
    Core_WaitOnObject(WAITABLE_OBJECT(*evnt));
    OBOS_Debug("Read %s. File contents:\n", udata[1]);
    printf("%*s\n", udata[3], udata[2]);
    Core_ExitCurrentThread();
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
    const char* const pathspec = "/test_folder/file.txt";
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
    char *buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, NULL, filesize, 0, VMA_FLAGS_PRIVATE, &file, nullptr);
    OBOS_Debug("Mapped %s. Contents:\n%*s\n", pathspec, filesize, buf);
    buf[0] = 'g';
    OBOS_Debug("Modified buffer. Contents:\n%*s\n", filesize, buf);
    // event *evnt = Vfs_Calloc(1, sizeof(event));
    // *evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    // OBOS_Debug("Reading %s\n", pathspec);
    // Vfs_FdARead(&file, buf, filesize, evnt);
    // thread* thr = CoreH_ThreadAllocate(nullptr);
    // thread_ctx ctx = {};
    // uintptr_t* udata = Vfs_Calloc(4, sizeof(uintptr_t));
    // udata[0] = (uintptr_t)evnt;
    // udata[1] = (uintptr_t)pathspec;
    // udata[2] = (uintptr_t)buf;
    // udata[3] = (uintptr_t)filesize;
    // CoreS_SetupThreadContext(
    //     &ctx, 
    //     (uintptr_t)test_thread, (uintptr_t)udata,
    //     false,
    //     Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x10000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr), 
    //     0x10000);
    // CoreH_ThreadInitialize(thr, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    // thr->stackFree = CoreH_VMAStackFree;
    // thr->stackFreeUserdata = &Mm_KernelContext;
    // OBOS_Debug("Starting thread %d.\n", thr->tid);
    // CoreH_ThreadReady(thr);
    end:
    if (root_partid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_partid, strlen(root_partid));
    if (root_uuid)
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, root_uuid, strlen(root_uuid));
}