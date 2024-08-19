/*
 * oboskrnl/vfs/pagecache.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "mm/bare_map.h"
#include "mm/pmm.h"
#include <int.h>
#include <memmanip.h>
#include <klog.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <vfs/alloc.h>
#include <vfs/pagecache.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <mm/alloc.h>
#include <mm/context.h>

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
    dirty->owner = pc;
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
    size_t filesize = newSize;
    newSize = newSize + (OBOS_PAGE_SIZE-(newSize%OBOS_PAGE_SIZE));
    Core_MutexAcquire(&pc->lock);
    size_t oldSz = pc->sz;
    // pc->sz = newSize;
    void* oldData = pc->data;
    pc->data = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, newSize, 0, VMA_FLAGS_NON_PAGED, nullptr, nullptr);
    if (oldData)
    {
        memcpy(pc->data, oldData, oldSz);
        Mm_VirtualMemoryFree(&Mm_KernelContext, oldData, oldSz);
    }
    page what = {};
    for (pagecache_mapped_region* curr = LIST_GET_HEAD(mapped_region_list, &pc->mapped_regions); curr && oldData; )
    {
        for (uintptr_t addr = curr->addr; addr < (curr->addr + curr->sz); addr += OBOS_PAGE_SIZE)
        {
            what.addr = addr;
            page* const pg = RB_FIND(page_tree, &curr->ctx->pages, &what);
            if (pg->isPrivateMapping)
            {
                curr = LIST_GET_NEXT(mapped_region_list, &pc->mapped_regions, curr);
                break;
            }
            OBOS_ASSERT(pg->region == curr);
            uintptr_t phys = 0;
            OBOSS_GetPagePhysicalAddress((void*)oldData + curr->fileoff, &phys);
            if (curr->fileoff >= pc->sz)
            {
                if (vn->filesize >= curr->fileoff)
                    phys = Mm_AllocatePhysicalPages(1, 1, nullptr); // TODO: Do some sort of CoW?
                else
                {
                    pg->prot.present = false;
                    phys = 0;
                }
            }
            MmS_SetPageMapping(curr->ctx->pt, pg, phys);
        }

        curr = LIST_GET_NEXT(mapped_region_list, &pc->mapped_regions, curr);
    }
    pc->sz = filesize;
    if (filesize > oldSz)
        vn->mount_point->fs_driver->driver->header.ftable.read_sync(vn->desc, pc->data, filesize-oldSz, oldSz, nullptr);
    Core_MutexRelease(&pc->lock);
}