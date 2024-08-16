/*
 * oboskrnl/vfs/pagecache.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <vfs/alloc.h>
#include <vfs/pagecache.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <driver_interface/header.h>

#include <stdatomic.h>

LIST_GENERATE(dirty_pc_list, struct pagecache_dirty_region, node);
pagecache_dirty_region* VfsH_PCDirtyRegionLookup(pagecache* pc, size_t off)
{
    Core_MutexAcquire(&pc->dirty_list_lock);
    for (pagecache_dirty_region* curr = LIST_GET_HEAD(dirty_pc_list, &pc->dirty_regions); curr; )
    {   
        if (off >= curr->fileoff && off < (curr->fileoff + curr->sz))
        {
            Core_MutexRelease(&pc->dirty_list_lock);
            return curr;
        }

        curr = LIST_GET_NEXT(dirty_pc_list, &pc->dirty_regions, curr);
    }
    Core_MutexRelease(&pc->dirty_list_lock);
    return nullptr;
}
// Note!
// Does a lookup first, and if there is already a dirty region that can fit the contraints passed, it is used.
// If one contains the offset, but is too small, it is expanded.
// Otherwise, a new region is made.
// This returns the dirty region created.
pagecache_dirty_region* VfsH_PCDirtyRegionCreate(pagecache* pc, size_t off, size_t sz)
{
    OBOS_ASSERT(!(off >= pc->sz || (off+sz) >= pc->sz));
    if (off >= pc->sz || (off+sz) >= pc->sz)
        return nullptr; // impossible for this to happen in normal cases.
    pagecache_dirty_region* dirty = VfsH_PCDirtyRegionLookup(pc, off);
    if (dirty)
    {
        if ((dirty->fileoff + off + sz) <= (dirty->fileoff + dirty->sz))
            return dirty; // we have space in this dirty region, return it
        // not enough space, expand the region.
        size_t new_cap = (dirty->fileoff + off + sz);
        // TODO(oberrow): Does this need to be synchronized
        dirty->sz = new_cap;
        return dirty;
    }
    dirty = Vfs_Calloc(1, sizeof(pagecache_dirty_region));
    dirty->fileoff = off;
    dirty->sz = sz;
    Core_MutexAcquire(&pc->dirty_list_lock);
    LIST_APPEND(dirty_pc_list, &pc->dirty_regions, dirty);
    Core_MutexRelease(&pc->dirty_list_lock);
    return dirty;
}
void VfsH_PageCacheRef(pagecache* pc)
{
    pc->refcnt++;
}
void VfsH_PageCacheUnref(pagecache* pc)
{
    pc->refcnt--;
    if (!pc->refcnt)
    {
        Vfs_Free(pc->data);
        pc->data = nullptr;
        pc->sz = 0;
    }
}
void VfsH_PageCacheFlush(pagecache* pc, void* vn_)
{
    vnode* vn = (vnode*)vn_;
    OBOS_ASSERT(vn);
    OBOS_ASSERT(&vn->pagecache == pc);
    Core_MutexAcquire(&pc->dirty_list_lock);
    driver_id* driver = vn->mount_point->fs_driver->driver;
    for (pagecache_dirty_region* curr = LIST_GET_HEAD(dirty_pc_list, &pc->dirty_regions); curr; )
    {   
        pagecache_dirty_region* next = LIST_GET_NEXT(dirty_pc_list, &pc->dirty_regions, curr);
        driver->header.ftable.write_sync(vn->desc, pc->data + curr->fileoff, curr->sz, curr->fileoff, nullptr);
        curr = next;
    }
    Core_MutexRelease(&pc->dirty_list_lock);
}
void VfsH_PageCacheResize(pagecache* pc, void* vn_, size_t newSize)
{
    vnode* vn = (vnode*)vn_;
    OBOS_ASSERT(newSize <= vn->filesize);
    if (newSize == pc->sz)
        return;
    Core_MutexAcquire(&pc->lock);
    size_t oldSz = pc->sz;
    pc->sz = newSize;
    pc->data = Vfs_Realloc(pc->data, pc->sz);
    if (pc->sz > oldSz)
        vn->mount_point->fs_driver->driver->header.ftable.read_sync(vn->desc, pc->data, pc->sz-oldSz, oldSz, nullptr);
    Core_MutexRelease(&pc->lock);
}