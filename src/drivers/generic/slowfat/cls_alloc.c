/*
 * drivers/generic/slowfat/cls_alloc.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <locks/mutex.h>

#include <vfs/fd.h>

#include <allocators/base.h>

#include "structs.h"
#include "alloc.h"

static bool isClusterFree(fat_cache* volume, uint32_t cluster, uint8_t* sector)
{
    bool res = false;
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    if (Vfs_FdTellOff(volume->volume) != (addr.lba*volume->blkSize))
    {
        Vfs_FdSeek(volume->volume, addr.lba*volume->blkSize, SEEK_SET);
        Vfs_FdRead(volume->volume, sector, volume->blkSize, nullptr);
    }   
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
            fat12_entry ent = GetFat12Entry(*(uint16_t*)(sector + addr.offset), cluster);
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
static void markAllocated(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    uint8_t* sector = FATAllocator->ZeroAllocate(FATAllocator, 1, volume->blkSize, nullptr);
    Vfs_FdSeek(volume->volume, addr.lba*volume->blkSize, SEEK_SET);
    Vfs_FdRead(volume->volume, sector, volume->blkSize, nullptr);
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
    for (size_t i = 0; i < volume->bpb->nFATs; i++)
    {
        Vfs_FdSeek(volume->volume, (addr.lba+volume->fatSz*i)*volume->blkSize, SEEK_SET);
        Vfs_FdWrite(volume->volume, sector, volume->blkSize, nullptr);
    }
    FATAllocator->Free(FATAllocator, sector, volume->blkSize);
}
static void markFree(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    uint8_t* sector = FATAllocator->ZeroAllocate(FATAllocator, 1, volume->blkSize, nullptr);
    Vfs_FdSeek(volume->volume, addr.lba*volume->blkSize, SEEK_SET);
    Vfs_FdRead(volume->volume, sector, volume->blkSize, nullptr);
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
    for (size_t i = 0; i < volume->bpb->nFATs; i++)
    {
        Vfs_FdSeek(volume->volume, (addr.lba+volume->fatSz*i)*volume->blkSize, SEEK_SET);
        Vfs_FdWrite(volume->volume, sector, volume->blkSize, nullptr);
    }
    FATAllocator->Free(FATAllocator, sector, volume->blkSize);
}
static void markEnd(fat_cache* volume, uint32_t cluster)
{
    fat_entry_addr addr = {};
    GetFatEntryAddrForCluster(volume, cluster, &addr);
    uint8_t* sector = FATAllocator->ZeroAllocate(FATAllocator, 1, volume->blkSize, nullptr);
    Vfs_FdSeek(volume->volume, addr.lba*volume->blkSize, SEEK_SET);
    Vfs_FdRead(volume->volume, sector, volume->blkSize, nullptr);
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
    for (size_t i = 0; i < volume->bpb->nFATs; i++)
    {
        Vfs_FdSeek(volume->volume, (addr.lba+volume->fatSz*i)*volume->blkSize, SEEK_SET);
        Vfs_FdWrite(volume->volume, sector, volume->blkSize, nullptr);
    }
    FATAllocator->Free(FATAllocator, sector, volume->blkSize);
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
    uint8_t* sector = FATAllocator->ZeroAllocate(FATAllocator, 1, volume->blkSize, nullptr);
    for (; !isLastCluster(volume, cluster); cluster++)
    {
        if (isClusterFree(volume, cluster, sector))
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
    FATAllocator->Free(FATAllocator, sector, volume->blkSize);
}