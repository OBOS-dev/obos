/*
 * oboskrnl/vfs/pagecache.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <klog.h>
#include <partition.h>

#include <locks/event.h>

#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/swap.h>

#include <vfs/irp.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <driver_interface/header.h>

static inline page* VfsH_PageCacheCreateEntry(vnode* vn, size_t offset)
{
    if (vn->flags & VFLAGS_FB)
        return nullptr;
    vn->refs++;
    page* phys = MmH_PgAllocatePhysical(false, false);
    phys->backing_vn = vn;
    phys->file_offset = offset;
    RB_INSERT(pagecache_tree, &Mm_Pagecache, phys);
    mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
        driver = &vn->un.device->driver->header;
    if (!driver) return nullptr;
    if (!vn->blkSize)
    {
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);
        OBOS_ASSERT(vn->blkSize);
    }
    const size_t base_offset = vn->flags & VFLAGS_PARTITION ? (vn->partitions[0].off/vn->blkSize) : 0;
    OBOS_ENSURE(obos_is_success(driver->ftable.read_sync(vn->desc, MmS_MapVirtFromPhys(phys->phys), OBOS_PAGE_SIZE / vn->blkSize, offset+base_offset, nullptr)));
    return phys;
}
static inline void* VfsH_PageCacheGetEntry(vnode* vn, size_t offset, page** ent)
{
    if (!vn->blkSize)
    {
        mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
        driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
        if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
            driver = &vn->un.device->driver->header;
        if (!driver) return nullptr;
        driver->ftable.get_blk_size(vn->desc, &vn->blkSize);
        OBOS_ASSERT(vn->blkSize);
    }
    uintptr_t pg_offset = offset % OBOS_PAGE_SIZE;
    offset -= (offset % OBOS_PAGE_SIZE);
    offset /= vn->blkSize;
    page key = {.file_offset=offset, .backing_vn=vn};
    page* phys = RB_FIND(pagecache_tree, &Mm_Pagecache, &key);
    if (!phys)
    {
        phys = VfsH_PageCacheCreateEntry(vn, offset);
        if (!phys)
            return nullptr;
        if (ent)
            *ent = phys;
        return MmS_MapVirtFromPhys(phys->phys) + pg_offset;
    }
    if (ent)
        *ent = phys;
    return MmS_MapVirtFromPhys(phys->phys) + pg_offset;
}