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

#include <utils/string.h>

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
    {
        dirent* child1 = Vfs_Calloc(1, sizeof(dirent));
        dirent* child2 = Vfs_Calloc(1, sizeof(dirent));
        dirent* child3 = Vfs_Calloc(1, sizeof(dirent));
        dirent* child4 = Vfs_Calloc(1, sizeof(dirent));
        dirent* child4_1 = Vfs_Calloc(1, sizeof(dirent));
        dirent* child5 = Vfs_Calloc(1, sizeof(dirent));
        OBOS_InitString(&child1->name, "dir");
        OBOS_InitString(&child2->name, "test");
        OBOS_InitString(&child3->name, "other_test");
        OBOS_InitString(&child4->name, "last_test");
        OBOS_InitString(&child4_1->name, "test2");
        OBOS_InitString(&child5->name, "actual_last_test");
        VfsH_DirentAppendChild(Vfs_Root, child1);
        VfsH_DirentAppendChild(child1, child2);
        VfsH_DirentAppendChild(child2, child3);
        VfsH_DirentAppendChild(child3, child4);
        VfsH_DirentAppendChild(child3, child4_1);
        VfsH_DirentAppendChild(child4, child5);
    }
    dirent* name1 = VfsH_DirentLookup("/dir");
    if (name1)
        OBOS_Debug("Found dirent at /%s.\n", OBOS_GetStringCPtr(&name1->name));
    dirent* name2 = VfsH_DirentLookup("/dir/test");
    if (name2)
        OBOS_Debug("Found dirent at /%s/%s.\n", OBOS_GetStringCPtr(&name1->name), OBOS_GetStringCPtr(&name2->name));
    dirent* name3 = VfsH_DirentLookup("/dir/test/other_test/");
    if (name3)
        OBOS_Debug("Found dirent at /%s/%s/%s.\n", OBOS_GetStringCPtr(&name1->name), OBOS_GetStringCPtr(&name2->name), OBOS_GetStringCPtr(&name3->name));
    dirent* name4 = VfsH_DirentLookup("/dir/test/other_test/last_test");
    if (name4)
        OBOS_Debug("Found dirent at /%s/%s/%s/%s.\n", OBOS_GetStringCPtr(&name1->name), OBOS_GetStringCPtr(&name2->name), OBOS_GetStringCPtr(&name3->name), OBOS_GetStringCPtr(&name4->name));
    dirent* name4_1 = VfsH_DirentLookup("/dir/test/other_test/test2");
    if (name4_1)
        OBOS_Debug("Found dirent at /%s/%s/%s/%s.\n", OBOS_GetStringCPtr(&name1->name), OBOS_GetStringCPtr(&name2->name), OBOS_GetStringCPtr(&name3->name), OBOS_GetStringCPtr(&name4_1->name));
    dirent* name5 = VfsH_DirentLookup("/dir/test/other_test/last_test/actual_last_test");
    if (name5)
        OBOS_Debug("Found dirent at /%s/%s/%s/%s/%s.\n", OBOS_GetStringCPtr(&name1->name), OBOS_GetStringCPtr(&name2->name), OBOS_GetStringCPtr(&name3->name), OBOS_GetStringCPtr(&name4->name), OBOS_GetStringCPtr(&name5->name));
}