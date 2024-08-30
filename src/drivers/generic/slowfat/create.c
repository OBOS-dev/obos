/*
 * drivers/generic/slowfat/create.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/header.h>

#include <vfs/fd.h>

#include <locks/mutex.h>

#include "alloc.h"
#include "structs.h"

OBOS_WEAK obos_status mk_file(dev_desc* newDesc, dev_desc parent, const char* name, file_type type);
OBOS_WEAK obos_status move_desc_to(dev_desc desc, const char* where);
#define DIRENT_FREE 0xe5
obos_status remove_file(dev_desc desc)
{
    if (!desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    if (cache_entry->fdc_children.nChildren)
        return OBOS_STATUS_IN_USE; // we cannot remove a directory with children.
    fat_cache* cache = cache_entry->owner;
    /*
        To remove a file we need to:
            - remove the dirent
            - remove all LFN entries associated with it
            - remove the cache entry associated with it
            - free any clusters used by it
    */
    uint8_t* sector = FATAllocator->Allocate(FATAllocator, cache->blkSize, nullptr);
    off_t sector_offset = cache_entry->dirent_offset;
    if (sector_offset == 0)
        Vfs_FdSeek(cache->volume, -512L, SEEK_CUR);
    Vfs_FdSeek(cache->volume, cache_entry->dirent_lba*cache->blkSize, SEEK_SET);
    Vfs_FdRead(cache->volume, sector, cache->blkSize, nullptr);
    Vfs_FdSeek(cache->volume, -(int64_t)cache->blkSize, SEEK_CUR);

    fat_dirent* curr = (fat_dirent*)(sector + cache_entry->dirent_offset);
    curr--;
    bool wroteback = false;
    while (curr->attribs & LFN)
    {
        wroteback = false;
        curr->filename_83[0] = DIRENT_FREE;
        curr--;
        sector_offset -= sizeof(fat_dirent);
        if (sector_offset == 0)
        {
            sector_offset = cache->blkSize - 16;
            Vfs_FdWrite(cache->volume, sector, cache->blkSize, nullptr);
            Vfs_FdSeek(cache->volume, -512, SEEK_CUR);
            Vfs_FdRead(cache->volume, sector, cache->blkSize, nullptr);
            Vfs_FdSeek(cache->volume, -(int64_t)cache->blkSize, SEEK_CUR);
            wroteback = true;
            curr = (fat_dirent*)(sector + cache_entry->dirent_offset);
            curr--;
        }
    }
    cache_entry->data.filename_83[0] = DIRENT_FREE;
    if (!wroteback)
        Vfs_FdWrite(cache->volume, sector, cache->blkSize, nullptr);
    Vfs_FdFlush(cache->volume);
    WriteFatDirent(cache, cache_entry);
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    const uint32_t szClusters = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
    FreeClusters(cache, (uint32_t)cache_entry->data.first_cluster_low|((uint32_t)cache_entry->data.first_cluster_high<<16), szClusters);
    CacheRemoveChild(cache_entry->fdc_parent, cache_entry);
    FATAllocator->Free(FATAllocator, cache_entry, sizeof(*cache_entry));
    Vfs_FdFlush(cache->volume);
    return OBOS_STATUS_SUCCESS;
}