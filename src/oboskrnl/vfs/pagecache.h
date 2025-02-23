/*
 * oboskrnl/vfs/pagecache.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <klog.h>

#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/swap.h>

#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <driver_interface/header.h>

static inline page* VfsH_PageCacheCreateEntry(vnode* vn, size_t offset, bool mark_dirty)
{
    page* phys = MmH_PgAllocatePhysical(false, false);
    phys->backing_vn = vn;
    phys->file_offset = offset;
    RB_INSERT(pagecache_tree, &Mm_Pagecache, phys);
    if (mark_dirty)
        Mm_MarkAsDirtyPhys(phys);
    else
        Mm_MarkAsStandbyPhys(phys);
    mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK)
        driver = &vn->un.device->driver->header;
    if (!vn->blkSize)
    {
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);
        OBOS_ASSERT(vn->blkSize);
    }
    driver->ftable.read_sync(vn->desc, MmS_MapVirtFromPhys(phys->phys), OBOS_PAGE_SIZE / vn->blkSize, offset, nullptr);
    return phys;
}
static inline void* VfsH_PageCacheGetEntry(vnode* vn, size_t offset, bool mark_dirty)
{
    if (!vn->blkSize)
    {
        mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
        driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
        if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK)
            driver = &vn->un.device->driver->header;
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);
        OBOS_ASSERT(vn->blkSize);
    }
    offset *= vn->blkSize;
    offset -= (offset % OBOS_PAGE_SIZE);
    page key = {.file_offset=offset, .backing_vn=vn};
    page* phys = RB_FIND(pagecache_tree, &Mm_Pagecache, &key);
    if (!phys)
    {
        phys = VfsH_PageCacheCreateEntry(vn, offset, mark_dirty);
        return MmS_MapVirtFromPhys(phys->phys);
    }
    if (mark_dirty)
        Mm_MarkAsDirtyPhys(phys);
    else
        Mm_MarkAsStandbyPhys(phys);
    return MmS_MapVirtFromPhys(phys->phys);
}