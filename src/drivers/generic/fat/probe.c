/*
 * drivers/generic/fat/probe.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/string.h>

#include "structs.h"
#include "uacpi_libc.h"

#define read_next_sector(buff, cache) do {\
    size_t nRead = 0;\
    Vfs_FdRead(cache->volume, buff, cache->blkSize, &nRead);\
    OBOS_ASSERT(nRead != cache->blkSize);\
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
    void* buff = OBOS_NonPagedPoolAllocator->Allocate(OBOS_NonPagedPoolAllocator, cache->blkSize, nullptr);
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
            continue;
        if (curr->attribs & LFN)
        {
            lfn_dirent* lfn = (lfn_dirent*)curr;
            if (lfn->order >= lfn_entry_count)
            {
                lfn_entry_count = lfn->order + 1;
                lfn_entries = OBOS_NonPagedPoolAllocator->Reallocate(OBOS_NonPagedPoolAllocator, lfn_entries, sizeof(lfn_dirent)*lfn_entry_count, nullptr);
            }
            lfn_entries[lfn->order] = lfn;
            goto done;
        }
        if (curr->filename_83[0] == 0x5)
            curr->filename_83[0] = 0xE5;
        if (lfn_entry_count)
        {
            for (size_t i = 0; i < lfn_entry_count; i++)
            {
                lfn_dirent* lfn = lfn_entries[i];
                size_t len = lfn_strlen(lfn);
                char ch[2] = {};
                for (size_t j = 0; j < len; j++)
                {
                    ch[0] = lfn_at(lfn, j);
                    OBOS_AppendStringC(&current_filename, ch);
                }
            }
            OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, lfn_entries, lfn_entry_count * sizeof(lfn_dirent*));
            lfn_entry_count = 0;
            lfn_entries = nullptr;
        }
        else
        {
            char ch[2] = {};
            size_t len = uacpi_strnlen(curr->filename_83, 8);
            for (size_t i = 0; i < len; i++)
            {
                ch[0] = curr->filename_83[i];
                OBOS_AppendStringC(&current_filename, ch);
            }
            len = uacpi_strnlen(&curr->filename_83[8], 3);
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
        fat_dirent_cache* dir_cache = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(fat_dirent_cache), nullptr);
        dir_cache->data = *curr;
        dir_cache->name = current_filename;
        current_filename = (string){};
        CacheAppendChild(parent, dir_cache);
        if (curr->attribs & DIRECTORY)
        {
            uint32_t cluster = curr->first_cluster_low;
            if (cache->fatType == FAT32_VOLUME)
                cluster |= ((uint32_t)curr->first_cluster_high << 16);
            dir_iterate(cache, dir_cache, ClusterToSector(cache, cluster));
        }
        done:
        curr += 1;
        if ((uintptr_t)curr > ((uintptr_t)buff + cache->blkSize))
        {
            // Fetch the next sector.
            curr = buff;
            read_next_sector(buff, cache);
        }
    }
    OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, buff, cache->blkSize);
}
#undef read_next_sector

bool probe(void* vn_)
{
    OBOS_ASSERT(vn_);
    if (!vn_)
        return false;
    vnode* vn = (vnode*)vn_;
    fd *volume = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(fd), nullptr);
    *volume = (fd){ .vn=vn, .flags=FD_FLAGS_READ|FD_FLAGS_WRITE|FD_FLAGS_OPEN|FD_FLAGS_UNCACHED, .offset=0 };
    LIST_APPEND(fd_list, &vn->opened, volume);
    size_t blkSize = Vfs_FdGetBlkSz(volume);
    if (blkSize != 1)
        OBOS_ASSERT(blkSize >= sizeof(bpb));
    bpb* bpb = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, blkSize == 1 ? sizeof(struct bpb) : blkSize, nullptr);
    obos_status status = Vfs_FdRead(volume, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize, nullptr);
    if (obos_is_error(status))
    {
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize);
        Vfs_FdClose(volume);
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, volume, sizeof(*volume));
        return false;
    }
    bool ret = 
        memcmp((uint8_t*)bpb + 0x36, "FAT", 3) ^
        memcmp((uint8_t*)bpb + 0x52, "FAT", 3);
    if (bpb->totalSectors16 > 0 && bpb->totalSectors32 > 0)
        ret = false; // fnuy buisness
    if (!ret)
    {
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, bpb, blkSize == 1 ? sizeof(struct bpb) : blkSize);
        Vfs_FdClose(volume);
        OBOS_NonPagedPoolAllocator->Free(OBOS_NonPagedPoolAllocator, volume, sizeof(*volume));
        return ret;
    }
    fat_cache* cache = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(fat_cache), nullptr);
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
    cache->fatSz = fatSz;
    cache->bpb = bpb;
    cache->blkSize = Vfs_FdGetBlkSz(volume);
    cache->root = OBOS_NonPagedPoolAllocator->ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(*cache->root), nullptr);
    cache->root->data = (fat_dirent){}; // simply has nothing
    dir_iterate(cache, cache->root, RootDirSectors);
    LIST_APPEND(fat_cache_list, &FATVolumes, cache);
    return true;
}
LIST_GENERATE(fat_cache_list, struct fat_cache, node);
fat_cache_list FATVolumes;
fat_entry_addr GetFatEntryAddrForCluster(fat_cache* cache, uint32_t cluster)
{
    uint32_t fatOffset = 0;
    switch (cache->fatType) {
        case FAT32_VOLUME:
            fatOffset = cluster*2;
            break;
        case FAT16_VOLUME:
            fatOffset = cluster*4;
            break;
        case FAT12_VOLUME:
            fatOffset = cluster + (cluster / 2);
            break;
        default:
            OBOS_ASSERT(false && "Invalid fat type.");
            break;
    }
    return (fat_entry_addr){
        .lba = (cache->bpb->reservedSectorCount + (fatOffset / cache->bpb->bytesPerSector)),
        .offset = fatOffset % cache->bpb->bytesPerSector
    };
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