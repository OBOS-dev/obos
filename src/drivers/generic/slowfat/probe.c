/*
 * drivers/generic/slowfat/probe.c
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/limits.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/string.h>

#include "structs.h"
#include "alloc.h"
#include "vfs/pagecache.h"

#include <uacpi_libc.h>

#define read_next_sector(buff, cache) do {\
    size_t nRead = 0;\
    Vfs_FdRead(cache->volume, buff, cache->blkSize, &nRead);\
    OBOS_ASSERT(nRead == cache->blkSize);\
} while(0)
static char lfn_at(const lfn_dirent* lfn, size_t i)
{
    char ret = 0;
    if (i < 5)
        ret = lfn->name1[i];
    else if (i >= 5 && i < 11)
        ret = lfn->name2[i - 5];
    else if (i == 11 || i == 12)
        ret = lfn->name3[i - 11];
    if (ret < 0)
        ret = 0;
    return ret;
}
static size_t lfn_strlen(const lfn_dirent* lfn)
{
    size_t ret = 0;
    for (; lfn_at(lfn, ret) && ret < 13; ret++);
    return ret;
}
static void dir_iterate(fat_cache* cache, fat_dirent_cache* parent, uint32_t lba)
{
    void* buff = FATAllocator->Allocate(FATAllocator, cache->blkSize, nullptr);
    uoff_t oldOffset = Vfs_FdTellOff(cache->volume);
    Vfs_FdSeek(cache->volume, lba*cache->blkSize, SEEK_SET);
    read_next_sector(buff, cache);
    fat_dirent* curr = buff;
    string current_filename = {};
    lfn_dirent **lfn_entries = nullptr;
    size_t lfn_entry_count = 0;
    OBOS_InitString(&current_filename, "");
    while(curr->filename_83[0] != 0)
    {
        if ((uint8_t)curr->filename_83[0] == 0xE5)
            goto done;
        if (curr->attribs & LFN)
        {
            lfn_dirent* lfn = (lfn_dirent*)curr;
            // NOTE(oberrow): Do not check if there was already a chain before, just truncate the list.
            if ((lfn->order & 0x40))
            {
                // Allocate all the memory we'll need for this LFN chain.
                lfn_entry_count = (lfn->order & ~0x40 /* last entry */);
                lfn_entries = FATAllocator->Reallocate(FATAllocator, lfn_entries, sizeof(lfn_dirent)*lfn_entry_count, nullptr);
            }
            lfn_entries[(lfn->order & ~0x40) - 1] = lfn;
            goto done;
        }
        if (curr->filename_83[0] == 0x5)
            curr->filename_83[0] = 0xE5;
        
        if (memcmp_b(&curr->filename_83[0], '.', 2))
            goto done;
        if (curr->filename_83[0] == '.')
            goto done;
        if (lfn_entry_count)
        {
            for (size_t i = 0; i < lfn_entry_count; i++)
            {
                lfn_dirent* lfn = lfn_entries[i];
                if (!lfn)
                    continue;
                size_t len = lfn_strlen(lfn);
                char ch[2] = {};
                for (size_t j = 0; j < len; j++)
                {
                    ch[0] = lfn_at(lfn, j);
                    if (!ch[0])
                        continue;
                    OBOS_AppendStringC(&current_filename, ch);
                }
            }
            FATAllocator->Free(FATAllocator, lfn_entries, lfn_entry_count * sizeof(lfn_dirent*));
            lfn_entry_count = 0;
            lfn_entries = nullptr;
        }
        else
        {
            char ch[2] = {};
            size_t len = 0;
            for (len = 0; len < 8 && curr->filename_83[len] != ' ' && curr->filename_83[len]; len++)
                ;
            for (size_t i = 0; i < len; i++)
            {
                ch[0] = curr->filename_83[i];
                OBOS_AppendStringC(&current_filename, ch);
            }
            for (len = 0; len < 3 && curr->filename_83[len+8] != ' ' && curr->filename_83[len+8]; len++)
                ;
            if (len != 0)
            {
                ch[0] = '.';
                OBOS_AppendStringC(&current_filename, ch);
                for (size_t i = 0; i < len; i++)
                {
                    ch[0] = curr->filename_83[i+8];
                    OBOS_AppendStringC(&current_filename, ch);
                }
            }
        }
        fat_dirent_cache* dir_cache = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(fat_dirent_cache), nullptr);
        dir_cache->data = *curr;
        dir_cache->name = current_filename;
        dir_cache->owner = cache;
        dir_cache->dirent_lba = lba;
        dir_cache->dirent_offset = ((uintptr_t)curr-(uintptr_t)buff);
        OBOS_InitStringLen(&dir_cache->path, OBOS_GetStringCPtr(&parent->path), OBOS_GetStringSize(&parent->path));
        if (OBOS_GetStringSize(&dir_cache->path))
            OBOS_AppendStringC(&dir_cache->path, "/");
        OBOS_AppendStringS(&dir_cache->path, &dir_cache->name);
        current_filename = (string){};
        CacheAppendChild(parent, dir_cache);
        // OBOS_Debug("%s\n", OBOS_GetStringCPtr(&dir_cache->path));
        if (curr->attribs & DIRECTORY)
        {
            uint32_t cluster = curr->first_cluster_low;
            if (cache->fatType == FAT32_VOLUME)
                cluster |= ((uint32_t)curr->first_cluster_high << 16);
            dir_iterate(cache, dir_cache, ClusterToSector(cache, cluster));
        }
        done:
        curr += 1;
        if ((uintptr_t)curr >= ((uintptr_t)buff + cache->blkSize))
        {
            // Fetch the next sector.
            curr = buff;
            read_next_sector(buff, cache);
        }
    }
    FATAllocator->Free(FATAllocator, buff, cache->blkSize);
    Vfs_FdSeek(cache->volume, oldOffset, SEEK_SET);
}
#undef read_next_sector

bool probe(void* vn_)
{
    OBOS_ASSERT(vn_);
    if (!vn_)
        return false;
    vnode* vn = (vnode*)vn_;
    fd *volume = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(fd), nullptr);
    *volume = (fd){ .vn=vn, .flags=FD_FLAGS_READ|FD_FLAGS_WRITE|FD_FLAGS_OPEN, .offset=0 };
    LIST_APPEND(fd_list, &vn->opened, volume);
    size_t blkSize = Vfs_FdGetBlkSz(volume);
    if (blkSize != 1)
        OBOS_ASSERT(blkSize >= sizeof(bpb));
    bpb* bpb = FATAllocator->ZeroAllocate(FATAllocator, 1, blkSize == 1 ? sizeof(struct bpb) : blkSize, nullptr);
    obos_status status = Vfs_FdRead(volume, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize, nullptr);
    if (obos_is_error(status))
    {
        FATAllocator->Free(FATAllocator, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize);
        Vfs_FdClose(volume);
        FATAllocator->Free(FATAllocator, volume, sizeof(*volume));
        return false;
    }
    bool ret = 
        memcmp((uint8_t*)bpb + 0x36, "FAT", 3) ^
        memcmp((uint8_t*)bpb + 0x52, "FAT", 3);
    if (bpb->totalSectors16 > 0 && bpb->totalSectors32 > 0)
        ret = false; // fnuy buisness
    if (!ret)
    {
        FATAllocator->Free(FATAllocator, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize);
        Vfs_FdClose(volume);
        FATAllocator->Free(FATAllocator, volume, sizeof(*volume));
        return ret;
    }
    fat_cache* cache = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(fat_cache), nullptr);
    cache->vn = vn;
    cache->volume = volume;
    uint32_t RootDirSectors = ((bpb->rootEntryCount * 32) + (bpb->bytesPerSector - 1)) / bpb->bytesPerSector;
    uint32_t fatSz = bpb->fatSz16 ? bpb->fatSz16 : bpb->ebpb.fat32.fatSz32;
    size_t totalSectors = bpb->totalSectors16 ? bpb->totalSectors16 : bpb->totalSectors32;
    uint32_t FirstDataSector = bpb->reservedSectorCount + (bpb->nFATs * fatSz) + RootDirSectors;
    cache->FirstDataSector = FirstDataSector;
    cache->RootDirSectors = RootDirSectors;
    uint32_t DataSec = totalSectors - FirstDataSector;
    size_t CountofClusters = DataSec / bpb->sectorsPerCluster;
    if (CountofClusters < 4085)
        cache->fatType = FAT12_VOLUME;
    else if (CountofClusters < 65525)
        cache->fatType = FAT16_VOLUME;
    else
        cache->fatType = FAT32_VOLUME;
    cache->bpb = bpb;
    cache->root_sector = cache->fatType == FAT32_VOLUME ? ClusterToSector(cache, bpb->ebpb.fat32.rootCluster) : (FirstDataSector-RootDirSectors);
    cache->fatSz = fatSz;
    cache->blkSize = Vfs_FdGetBlkSz(volume);
    if (cache->blkSize == 1)
        cache->blkSize = bpb->bytesPerSector;
    cache->CountofClusters = CountofClusters;
    cache->root = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(*cache->root), nullptr);
    VfsH_PageCacheResize(&cache->volume->vn->pagecache, cache->volume->vn, cache->FirstDataSector*cache->blkSize);
    cache->root->data = (fat_dirent){}; // simply has nothing
    OBOS_InitString(&cache->root->path, "");
    OBOS_InitString(&cache->root->name, "");
    dir_iterate(cache, cache->root, cache->root_sector);
    InitializeCacheFreelist(cache);
    LIST_APPEND(fat_cache_list, &FATVolumes, cache);
    OBOS_Debug("FAT: CountofClusters: 0x%08x\n", CountofClusters);
    OBOS_Debug("FAT: blkSize: 0x%08x\n", blkSize);
    OBOS_Debug("FAT: fatSz: 0x%08x\n", fatSz);
    OBOS_Debug("FAT: nFats: 0x%08x\n", cache->bpb->nFATs);
    return true;
}
LIST_GENERATE(fat_cache_list, struct fat_cache, node);
fat_cache_list FATVolumes;
void GetFatEntryAddrForCluster(fat_cache* cache, uint32_t cluster, fat_entry_addr* out)
{
    uint32_t fatOffset = 0;
    switch (cache->fatType) {
        case FAT32_VOLUME:
            fatOffset = cluster*4;
            break;
        case FAT16_VOLUME:
            fatOffset = cluster*2;
            break;
        case FAT12_VOLUME:
            fatOffset = cluster + (cluster / 2);
            break;
        default:
            OBOS_ASSERT(false && "Invalid fat type.");
            break;
    }
    fat_entry_addr addr = {};
    addr.lba = (cache->bpb->reservedSectorCount + fatOffset / cache->bpb->bytesPerSector);
    addr.offset = fatOffset % cache->bpb->bytesPerSector;
    *out = addr;
}
uint32_t GetClusterFromFatEntryAddr(fat_cache* cache, fat_entry_addr addr)
{
    return (addr.lba * cache->bpb->bytesPerSector - cache->bpb->reservedSectorCount) + addr.offset;
}
fat12_entry GetFat12Entry(uint16_t val, uint32_t valCluster)
{
    if (valCluster % 2)
        return (fat12_entry){ .ent=(val & 0xfff) };
    return (fat12_entry){ .ent=((val >> 4) & 0xfff) };
}
void CacheAppendChild(fat_dirent_cache* parent, fat_dirent_cache* child)
{
    if(!parent->fdc_children.head)
        parent->fdc_children.head = child;
    if (parent->fdc_children.tail)
        parent->fdc_children.tail->fdc_next_child = child;
    child->fdc_prev_child = parent->fdc_children.tail;
    parent->fdc_children.tail = child;
    parent->fdc_children.nChildren++;
    child->d_parent = parent;
}
void CacheRemoveChild(fat_dirent_cache* parent, fat_dirent_cache* what)
{
    if (what->fdc_prev_child)
        what->fdc_prev_child->fdc_next_child = what->fdc_next_child;
    if (what->fdc_next_child)
        what->fdc_next_child->fdc_prev_child = what->fdc_prev_child;
    if (parent->fdc_children.head == what)
        parent->fdc_children.head = what->fdc_next_child;
    if (parent->fdc_children.tail == what)
        parent->fdc_children.tail = what->fdc_prev_child;
    parent->fdc_children.nChildren--;
    what->d_parent = nullptr; // we're now an orphan :(
}