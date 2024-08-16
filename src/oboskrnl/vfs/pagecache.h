/*
 * oboskrnl/vfs/pagecache.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/list.h>

#include <locks/mutex.h>

#include <stdatomic.h>

typedef LIST_HEAD(dirty_pc_list, struct pagecache_dirty_region) dirty_pc_list;
LIST_PROTOTYPE(dirty_pc_list, struct pagecache_dirty_region, node);
typedef struct pagecache_dirty_region
{
    // take this lock when any reads or writes are done on the region this represents.
    // do not take it if expanding 'sz'
    mutex lock;
    size_t fileoff;
    atomic_size_t sz;
    LIST_NODE(dirty_pc_list, struct pagecache_dirty_region) node;
} pagecache_dirty_region;
typedef struct pagecache
{
    // Take this lock when expanding the page cache.
    mutex lock;
    char* data;
    size_t sz;
    // Take this lock when using dirty region list.
    mutex dirty_list_lock;
    dirty_pc_list dirty_regions;
    atomic_size_t refcnt;
} pagecache;
pagecache_dirty_region* VfsH_PCDirtyRegionLookup(pagecache* pc, size_t off);
// Note!
// Does a lookup first, and if there is already a dirty region that can fit the contraints passed, it is used.
// If one contains the offset, but is too small, it is expanded.
// Otherwise, a new region is made.
// This returns the dirty region created.
pagecache_dirty_region* VfsH_PCDirtyRegionCreate(pagecache* pc, size_t off, size_t sz);
// Adds a reference to the page cache
void VfsH_PageCacheRef(pagecache* pc); 
// Removes a reference from the page cache.
// If pc->ref reaches zero, the page cache is freed.
void VfsH_PageCacheUnref(pagecache* pc);
// Flushes the page cache.
// vn is of type `vnode*`
void VfsH_PageCacheFlush(pagecache* pc, void* vn);
// Resizes the page cache.
// vn is of type `vnode*`
void VfsH_PageCacheResize(pagecache* pc, void* vn, size_t newSize);