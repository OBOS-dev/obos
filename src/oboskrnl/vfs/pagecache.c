/*
 * oboskrnl/vfs/pagecache.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <klog.h>
#include <partition.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <vfs/alloc.h>
#include <vfs/pagecache.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/bare_map.h>
#include <mm/pmm.h>

#include <driver_interface/header.h>

#include <stdatomic.h>

LIST_GENERATE(dirty_pc_list, struct pagecache_dirty_region, node);
LIST_GENERATE(mapped_region_list, struct pagecache_mapped_region, node);
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
    // OBOS_ASSERT(!(off >= pc->sz || (off+sz) > pc->sz));
    // if (off >= pc->sz || (off+sz) > pc->sz)
    //     return nullptr; // impossible for this to happen in normal cases.
    pagecache_dirty_region* dirty = VfsH_PCDirtyRegionLookup(pc, off);
    if (dirty)
    {
        if ((dirty->fileoff + off + sz) <= (dirty->fileoff + dirty->sz))
            return dirty; // we have space in this dirty region, return it
        // not enough space, expand the region.
        size_t new_cap = (dirty->fileoff + off + sz);
        // TODO(oberrow): Does this need to be synchronized
        dirty->sz = new_cap;
        // OBOS_Debug("extending dirty region from offset 0x%016x to a size of 0x%016x bytes\n", dirty->fileoff, dirty->sz);
        return dirty;
    }
    dirty = Vfs_Calloc(1, sizeof(pagecache_dirty_region));
    dirty->fileoff = off;
    dirty->sz = sz;
    dirty->owner = pc;
    // OBOS_Debug("adding dirty region from offset 0x%016x with a size of 0x%016x bytes\n", dirty->fileoff, dirty->sz);
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
    }
}
void VfsH_PageCacheFlush(pagecache* pc, void* vn_)
{
    vnode* vn = (vnode*)vn_;
    OBOS_ASSERT(vn);
    OBOS_ASSERT(&vn->pagecache == pc);
    Core_MutexAcquire(&pc->dirty_list_lock);
    mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK)
        driver = &vn->un.device->driver->header;
    const size_t base_offset = vn->flags & VFLAGS_PARTITION ? vn->partitions[0].off : 0;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(vn->desc, &blkSize);
    for (pagecache_dirty_region* curr = LIST_GET_HEAD(dirty_pc_list, &pc->dirty_regions); curr; )
    {   
        pagecache_dirty_region* next = LIST_GET_NEXT(dirty_pc_list, &pc->dirty_regions, curr);
        // OBOS_Debug("flushing dirty region from offset 0x%016x with a size of 0x%016x bytes\n", curr->fileoff, curr->sz);
        driver->ftable.write_sync(vn->desc, pc->data + curr->fileoff, curr->sz/blkSize, (curr->fileoff+base_offset)/blkSize, nullptr);
        curr = next;
    }
    Core_MutexRelease(&pc->dirty_list_lock);
}
void *VfsH_PageCacheGetEntry(pagecache* pc, void* vn_, size_t offset, size_t size)
{
    vnode* vn = (vnode*)vn_;
    if (!pc->data)
        pc->data = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, vn->filesize, 0, VMA_FLAGS_RESERVE|VMA_FLAGS_NON_PAGED, nullptr, nullptr);
    // Formulate a list of each unmapped (and therefore, empty) page cache region that we need.
    uintptr_t* emptyPages = nullptr;
    size_t nEmptyPages = 0;
    uintptr_t base = (uintptr_t)pc->data + offset;
    base -= base % OBOS_PAGE_SIZE;
    uintptr_t top = base + size;
    // top += OBOS_PAGE_SIZE-(top%OBOS_PAGE_SIZE);
    page what = {};
    for (uintptr_t addr = base; addr < top; addr += OBOS_PAGE_SIZE)
    {
        what.addr = addr;
        page* curr = RB_FIND(page_tree, &Mm_KernelContext.pages, &what);
        OBOS_ASSERT(curr);
        if (curr->reserved)
        {
            emptyPages = Vfs_Realloc(emptyPages, ++nEmptyPages * sizeof(*emptyPages));
            emptyPages[nEmptyPages-1] = addr;
        }
    }
    mount* const point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK)
        driver = &vn->un.device->driver->header;
    size_t blkSize = 0;
    driver->ftable.get_blk_size(vn->desc, &blkSize);
    const size_t base_offset = vn->flags & VFLAGS_PARTITION ? vn->partitions[0].off : 0;
    for (size_t i = 0; i < nEmptyPages; i++)
    {
        Mm_VirtualMemoryAlloc(&Mm_KernelContext, (void*)emptyPages[i], OBOS_PAGE_SIZE, 0, VMA_FLAGS_NON_PAGED, nullptr, nullptr);
        const uintptr_t offset = ((emptyPages[i]-(uintptr_t)pc->data)+base_offset) / blkSize;
        obos_status status = driver->ftable.read_sync(vn->desc, (void*)emptyPages[i], OBOS_PAGE_SIZE/blkSize, offset, nullptr);
        if (obos_expect(obos_is_error(status) == true, 0))
            return nullptr;
    }
    Vfs_Free(emptyPages);
    return (void*)((uintptr_t)pc->data + offset);
}