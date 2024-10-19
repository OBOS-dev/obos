/*
 * oboskrnl/vfs/pagecache.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/list.h>
#include <utils/tree.h>

#include <locks/mutex.h>

#include <mm/handler.h>

#include <stdatomic.h>

typedef LIST_HEAD(mapped_region_list, struct pagecache_mapped_region) mapped_region_list;
LIST_PROTOTYPE(mapped_region_list, struct pagecache_mapped_region, node);
typedef RB_HEAD(dirty_pc_tree, pagecache_dirty_region) dirty_pc_tree;
RB_PROTOTYPE(dirty_pc_tree, pagecache_dirty_region, node, compare_dirty_regions);
typedef struct pagecache
{
    // Take this lock when expanding the page cache.
    mutex lock;
    char* data;
    struct page_range* cached_data_range;
    // Take this lock when using dirty region list.
    mutex dirty_list_lock;
    dirty_pc_tree dirty_regions;
    atomic_size_t refcnt;
    mapped_region_list mapped_regions;
    struct vnode* owner;
} pagecache;
typedef struct pagecache_dirty_region
{
    // take this lock when any reads or writes are done on the region this represents.
    // do not take it if expanding 'sz'
    mutex lock;
    size_t fileoff;
    atomic_size_t sz;
    pagecache* owner;
    RB_ENTRY(pagecache_dirty_region) node;
} pagecache_dirty_region;
typedef struct pagecache_mapped_region
{
    mutex lock;
    size_t fileoff;
    uintptr_t addr;
    atomic_size_t sz;
    pagecache* owner;
    struct context* ctx;
    LIST_NODE(mapped_region_list, struct pagecache_mapped_region) node;
} pagecache_mapped_region;
#define in_range(ra,rb,x) (((x) >= (ra)) && ((x) < (rb)))
inline static int compare_dirty_regions(pagecache_dirty_region* left, pagecache_dirty_region* right)
{
    if (in_range(right->fileoff, right->fileoff+right->sz, left->fileoff))
        return 0;
    if (left->fileoff < right->fileoff)
        return -1;
    if (left->fileoff > right->fileoff)
        return 1;
    return 0;
}
#undef in_range
OBOS_EXPORT pagecache_dirty_region* VfsH_PCDirtyRegionLookup(pagecache* pc, size_t off);
// Note!
// Does a lookup first, and if there is already a dirty region that can fit the contraints passed, it is used.
// If one contains the offset, but is too small, it is expanded.
// Otherwise, a new region is made.
// This returns the dirty region created.
OBOS_EXPORT pagecache_dirty_region* VfsH_PCDirtyRegionCreate(pagecache* pc, size_t off, size_t sz);
// Adds a reference to the page cache
OBOS_EXPORT void VfsH_PageCacheRef(pagecache* pc); 
// Removes a reference from the page cache.
// If pc->ref reaches zero, the page cache is freed.
OBOS_EXPORT void VfsH_PageCacheUnref(pagecache* pc);
// Flushes the page cache.
OBOS_EXPORT void VfsH_PageCacheFlush(pagecache* pc);
// Gets a page cache entry.
OBOS_EXPORT void *VfsH_PageCacheGetEntry(pagecache* pc, size_t offset, size_t size, fault_type* type);
