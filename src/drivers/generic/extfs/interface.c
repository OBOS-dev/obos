/*
 * drivers/generic/extfs/interface.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/page.h>
#include <mm/swap.h>

#include "structs.h"

#define get_handle(desc) ({\
    if (!desc) return OBOS_STATUS_INVALID_ARGUMENT;\
    (ext_inode_handle*)desc;\
})

obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    ext_inode_handle* hnd = get_handle(desc);
    page* pg = nullptr;
    ext_inode* ino = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);
    MmH_RefPage(pg);
    
    uint32_t new_mode = ino->mode & ~0777;
    
    if (newperm.other_exec)
        new_mode |= EXT_OTHER_EXEC;
    if (newperm.owner_exec)
        new_mode |= EXT_OWNER_EXEC;
    if (newperm.group_exec)
        new_mode |= EXT_GROUP_EXEC;

    if (newperm.other_write)
        new_mode |= EXT_OTHER_WRITE;
    if (newperm.owner_write)
        new_mode |= EXT_OWNER_WRITE;
    if (newperm.group_write)
        new_mode |= EXT_GROUP_WRITE;
    
    if (newperm.other_read)
        new_mode |= EXT_OTHER_READ;
    if (newperm.owner_read)
        new_mode |= EXT_OWNER_READ;
    if (newperm.group_read)
        new_mode |= EXT_GROUP_READ;

    ino->mode = new_mode;
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);

    return OBOS_STATUS_SUCCESS;
}
