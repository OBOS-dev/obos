/*
 * drivers/generic/slowfat/cls_alloc.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <locks/mutex.h>

#include <vfs/fd.h>

#include <allocators/base.h>

#include <driver_interface/header.h>

#include <mm/swap.h>

#include "structs.h"
#include "alloc.h"

#include <vfs/pagecache.h>
#include <vfs/vnode.h>

static OBOS_NO_UBSAN fat12_entry readFat12Entry(const uint8_t* sector, uint32_t cluster, fat_entry_addr addr)
{
    return GetFat12Entry(*(uint16_t*)(sector + addr.offset), cluster);
}
static bool isClusterFree(fat_cache* volume, uint32_t cluster)
{
    bool res = false;
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    const uint8_t* sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, nullptr);
    switch (volume->fatType)
    {
        case FAT32_VOLUME:
        {
            fat32_entry ent = {};
            memcpy(&ent, sector + addr.offset, sizeof(ent));
            res = !ent.ent;
            break;
        }
        case FAT16_VOLUME:
        {
            fat16_entry ent = {};
            memcpy(&ent, sector + addr.offset, sizeof(ent));
            res = !ent.ent;
            break;
        }
        case FAT12_VOLUME:
        {
            fat12_entry ent = readFat12Entry(sector, cluster, addr);
            res = !ent.ent;
            break;
        }
    }
    return res;
}
static bool isLastCluster(fat_cache* volume, uint32_t cluster)
{
    // return GetFatEntryAddrForCluster(volume, cluster).lba >= volume->fatSz;
    return cluster >= volume->CountofClusters;
}

static void sync_fat(fat_cache* volume, fat_entry_addr addr, uint8_t* sector)
{
    page* pg = nullptr;
    // Start at the 2nd FAT.
    for (size_t i = 1; i < volume->bpb->nFATs; i++)
    {
        memcpy(VfsH_PageCacheGetEntry(volume->volume->vn, (addr.lba+volume->fatSz*i)*volume->blkSize, &pg), sector, volume->blkSize);
        Mm_MarkAsDirtyPhys(pg);
    }
}

static void markAllocated(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    OBOS_ASSERT(volume->blkSize <= OBOS_PAGE_SIZE);
    page* pg = nullptr;
    uint8_t* sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, &pg);
    switch (volume->fatType)
    {
        case FAT32_VOLUME:
        {
            fat32_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            // note: +1 is intentional
            ent.ent = cluster+1;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT16_VOLUME:
        {
            fat16_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            // note: +1 is intentional
            ent.ent = cluster+1;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT12_VOLUME:
        {
            fat12_entry ent = GetFat12Entry(*(uint16_t*)(sector+addr.offset), cluster);
            // note: +1 is intentional
            ent.ent = cluster+1;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
    }
    Mm_MarkAsDirtyPhys(pg);
    sync_fat(volume, addr, sector);
}
static void markFree(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    page* pg = nullptr;
    uint8_t* sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, &pg);
    switch (volume->fatType)
    {
        case FAT32_VOLUME:
        {
            fat32_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            ent.ent = 0;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT16_VOLUME:
        {
            fat16_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            ent.ent = 0;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT12_VOLUME:
        {
            fat12_entry ent = GetFat12Entry(*(uint16_t*)(sector+addr.offset), cluster);
            ent.ent = 0;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
    }
    Mm_MarkAsDirtyPhys(pg);
    sync_fat(volume, addr, sector);
}
static void markEnd(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    page* pg = nullptr;
    uint8_t* sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, &pg);
    switch (volume->fatType)
    {
        case FAT32_VOLUME:
        {
            uint32_t free = 0x0FFFFFF8;
            fat32_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            ent.ent = free;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT16_VOLUME:
        {
            uint16_t free = 0xFFF8;
            fat16_entry ent = {};
            memcpy(&ent, sector+addr.offset, sizeof(ent));
            ent.ent = free;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
        case FAT12_VOLUME:
        {
            uint16_t free = 0x0FF8;
            fat12_entry ent = GetFat12Entry(*(uint16_t*)(sector+addr.offset), cluster);
            ent.ent = free;
            memcpy(sector+addr.offset, &ent, sizeof(ent));
            break;
        }
    }
    Mm_MarkAsDirtyPhys(pg);
    sync_fat(volume, addr, sector);
}
uint32_t AllocateClusters(fat_cache* volume, size_t nClusters)
{
    // TODO: Use fsinfo structure.
    // if (volume->fatType == FAT32_VOLUME)
    // {}
    if (volume->freelist.freeClusterCount < nClusters)
        return UINT32_MAX;
    fat_freenode* node = volume->freelist.tail;
    while (node && node->nClusters < nClusters)
        node = node->prev;
    if (!node)
        return UINT32_MAX;
    node->nClusters -= nClusters;
    uint32_t cluster = node->cluster+node->nClusters;
    if (!node->nClusters)
    {
        if (node->prev)
            node->prev->next = node->prev;
        if (node->next)
            node->next->prev = node->prev;
        if (volume->freelist.head == node)
            volume->freelist.head = node->next;
        if (volume->freelist.tail == node)
            volume->freelist.tail = node->prev;
        volume->freelist.nNodes--;
        FATAllocator->Free(FATAllocator, node, sizeof(*node));
    }
    for (size_t i = 0; i < nClusters; i++)
        markAllocated(volume, cluster+i);
    markEnd(volume, cluster+(nClusters-1));
    volume->freelist.freeClusterCount -= nClusters;
    return cluster;
}
// Returns true if the cluster region was extended, otherwise you need to reallocate the clusters.
bool ExtendClusters(fat_cache* volume, uint32_t cluster, size_t nClusters, size_t oldClusterCount)
{
    // if (nClusters < oldClusterCount)
    //     return false;
    // if (nClusters == oldClusterCount)
    //     return true;
    // size_t diff = nClusters-oldClusterCount;
    // size_t i = 0;
    // markFree(volume, cluster);
    // for (i = 0; i < diff && !isLastCluster(volume, cluster+i); i++)
    // {
    //     if (!isClusterFree(volume, cluster+i))
    //         return false;
    // };
    // for (i = 0; i < diff; i++)
    //     markAllocated(volume, cluster+i);
    // markEnd(volume, cluster+diff);
    // return true;
    OBOS_UNUSED(volume);
    OBOS_UNUSED(cluster);
    OBOS_UNUSED(nClusters);
    OBOS_UNUSED(oldClusterCount);
    return false; // TODO: Implement extending clusters.
}
void TruncateClusters(fat_cache* volume, uint32_t cluster, size_t newClusterCount, size_t oldClusterCount)
{
    if (newClusterCount >= oldClusterCount)
        return;
    FreeClusters(volume, cluster+oldClusterCount, oldClusterCount-newClusterCount);
    if (newClusterCount)
        markEnd(volume, cluster+newClusterCount-1);
}
void FreeClusters(fat_cache* volume, uint32_t cluster, size_t nClusters)
{
    for (size_t i = 0; i < nClusters && !isLastCluster(volume, cluster); i++)
        markFree(volume, cluster+i);
    fat_freenode *curr = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(*curr), nullptr);
    curr->cluster = cluster;
    curr->nClusters = nClusters;
    if (!volume->freelist.head)
        volume->freelist.head = curr;
    if (volume->freelist.tail)
        volume->freelist.tail->next = curr;
    curr->prev = volume->freelist.tail;
    volume->freelist.tail = curr;
    volume->freelist.nNodes++;
    volume->freelist.freeClusterCount += curr->nClusters;
}
void InitializeCacheFreelist(fat_cache* volume)
{
    uint32_t cluster = 0;
    struct fat_freenode* curr = nullptr;
    for (; !isLastCluster(volume, cluster); cluster++)
    {
        if (isClusterFree(volume, cluster))
        {
            if (!curr)
            {
                curr = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(*curr), nullptr);
                curr->cluster = cluster;
            }
            curr->nClusters++;
        }
        else
        {
            if (!curr)
                continue;
            while (isLastCluster(volume, curr->cluster + curr->nClusters))
                curr->nClusters--;
            if (!volume->freelist.head)
                volume->freelist.head = curr;
            if (volume->freelist.tail)
                volume->freelist.tail->next = curr;
            curr->prev = volume->freelist.tail;
            volume->freelist.tail = curr;
            volume->freelist.nNodes++;
            volume->freelist.freeClusterCount += curr->nClusters;
            curr = nullptr;
        }
    }
    if (curr)
    {
        if (!volume->freelist.head)
            volume->freelist.head = curr;
        if (volume->freelist.tail)
            volume->freelist.tail->next = curr;
        curr->prev = volume->freelist.tail;
        volume->freelist.tail = curr;
        volume->freelist.nNodes++;
        volume->freelist.freeClusterCount += curr->nClusters;
    }
}
obos_status NextCluster(fat_cache* cache, uint32_t cluster, uint8_t* sec_buf, uint32_t* ret)
{
    uint32_t res = 0;
    uint32_t last_clus_val = 0x0ffffff8;
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(cache, cluster, &addr);
    uint32_t offset = addr.offset;
    switch (cache->fatType) {
        case FAT32_VOLUME:
        {
            fat32_entry ent = {};
            memcpy(&ent, sec_buf + offset, sizeof(ent));
            res = ent.ent;
            last_clus_val = 0x0ffffff8;
            break;
        }
        case FAT16_VOLUME:
        {
            fat16_entry ent = {};
            memcpy(&ent, sec_buf + offset, sizeof(ent));
            res = ent.ent;
            last_clus_val = 0xfff8;
            break;
        }
        case FAT12_VOLUME:
        {
            fat12_entry ent = GetFat12Entry(*(uint16_t*)(sec_buf + offset), cluster);
            res = ent.ent;
            last_clus_val = 0xff8;
            break;
        }
    }
    *ret = res;
    return res >= last_clus_val ? OBOS_STATUS_EOF : OBOS_STATUS_SUCCESS;
}

static iterate_decision cluster_seek_cb(uint32_t cluster, obos_status status, void* userdata)
{
    uint32_t* data = userdata;
    uint32_t* const nClusters = data+1;
    uint32_t* const res = data;
    if (!(--(*nClusters)))
    {
        *res = cluster;
        return ITERATE_DECISION_STOP;
    }
    return ITERATE_DECISION_CONTINUE;
}
uint32_t ClusterSeek(fat_cache* cache, uint32_t cluster, uint32_t nClusters)
{
    if (!nClusters)
        return cluster;
    uint32_t udata[2] = {UINT32_MAX,nClusters};
    FollowClusterChain(cache, cluster, cluster_seek_cb, udata);
    return udata[0]; // If UINT32_MAX, then nClusters is out of bounds.
}

void FollowClusterChain(fat_cache* volume, uint32_t clus, clus_chain_cb callback, void* userdata)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, clus, &addr);
    uint8_t* sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, nullptr);
    uint32_t curr = clus;
    obos_status status = OBOS_STATUS_SUCCESS;
    do {
        if (callback(curr, status, userdata) == ITERATE_DECISION_STOP)
            break;
        status = NextCluster(volume, curr, sector, &curr);
        if (status == OBOS_STATUS_EOF)
            break;
        if (curr == 0)
        {
            OBOS_Error("FAT: Error following cluster chain: Unexpected free cluster. Aborting.\n");
            callback(0, OBOS_STATUS_ABORTED, userdata);
            break;
        }
        if (curr >= volume->CountofClusters)
        {
            OBOS_Error("FAT: Error following cluster chain: Cluster is over disk boundaries. Aborting.\n");
            callback(0, OBOS_STATUS_ABORTED, userdata);
            break;
        }
        uint64_t prev_lba = addr.lba;
        GetFatEntryAddrForCluster(volume, curr, &addr);
        if (addr.lba != prev_lba)
            sector = VfsH_PageCacheGetEntry(volume->volume->vn, addr.lba*volume->blkSize, nullptr);
    } while(status != OBOS_STATUS_EOF);
}
