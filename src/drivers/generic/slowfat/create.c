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

#include <utils/string.h>

#include "alloc.h"
#include "structs.h"
#include "vfs/pagecache.h"
#include "vfs/vnode.h"

bool valid_filename(const char* filename, bool isPath)
{
    for (size_t i = 0; filename[i]; i++)
    {
        switch (filename[i]) {
            case '<':
            case '>':
            case ':':
            case '\"':
            case '\\':
            case '|':
            case '?':
            case '*':
                return false;
            case '/':
            {
                if (!isPath)
                    return false;
                break;
            }
        }
    }
    return true;
}
static void ref_dirent(fat_dirent_cache* cache_entry);
static void gen_short_name(const char* long_name, char* name, fat_dirent_cache* parent, fat_dirent_cache* dirent);
obos_status mk_file(dev_desc* newDesc, dev_desc parent_desc, void* vn_, const char* name, file_type type)
{
    if (!parent_desc || !newDesc || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (type == FILE_TYPE_SYMBOLIC_LINK)
        return OBOS_STATUS_INTERNAL_ERROR;
    if (!valid_filename(name, false))
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* parent = (fat_dirent_cache*)parent_desc;
    fat_cache* cache = nullptr;
    if (parent_desc == UINTPTR_MAX)
    {
        if (!vn_)
            return OBOS_STATUS_INVALID_ARGUMENT;
        for (cache = LIST_GET_HEAD(fat_cache_list, &FATVolumes); cache; )
        {
            if (cache->vn == vn_)
                break;

            cache = LIST_GET_NEXT(fat_cache_list, &FATVolumes, cache);
        }
        if (!cache)
            return OBOS_STATUS_INVALID_OPERATION; // not a fat volume we have probed
        parent = cache->root;
    }
    if (!cache)
        cache = parent->owner;
    if (!(parent->data.attribs & DIRECTORY))
        return OBOS_STATUS_INVALID_ARGUMENT;
    for (fat_dirent_cache* curr = parent->fdc_children.head; curr; )
    {
        if (OBOS_CompareStringC(&curr->name, name))
            return OBOS_STATUS_ALREADY_INITIALIZED;
        curr = curr->fdc_next_child;
    }
    fat_dirent_cache* new = FATAllocator->ZeroAllocate(FATAllocator, 1, sizeof(fat_dirent_cache), nullptr);
    *newDesc = (dev_desc)new;
    cache->fileCount++;
    new->data.creation_date = (fat_date){};
    new->data.last_mod_date = new->data.creation_date;
    new->data.creation_time = (fat_time){};
    new->data.last_mod_time = new->data.creation_time;
    new->data.access_date = new->data.creation_date;
    new->data.filesize = 0;
    new->data.first_cluster_low = 0;
    new->data.first_cluster_high = 0;
    new->data.attribs |= (type == FILE_TYPE_DIRECTORY) ? DIRECTORY : 0;
    gen_short_name(name, new->data.filename_83, parent, new);
    new->owner = parent->owner;
    OBOS_InitString(&new->name, name);
    OBOS_InitStringLen(&new->path, OBOS_GetStringCPtr(&parent->path), OBOS_GetStringSize(&parent->path));
    if (OBOS_GetStringSize(&new->path))
        if (OBOS_GetStringCPtr(&new->path)[OBOS_GetStringSize(&new->path) - 1] != '/')
            OBOS_AppendStringC(&new->path, "/");
    OBOS_AppendStringS(&new->path, &new->name);
    CacheAppendChild(parent, new);
    Core_MutexAcquire(&new->owner->fd_lock);
    ref_dirent(new);
    Vfs_FdFlush(new->owner->volume);
    Core_MutexRelease(&new->owner->fd_lock);
    return OBOS_STATUS_SUCCESS;
}
static size_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return SIZE_MAX;
}
static char toupper(char ch)
{
    if (ch >= 'a' && ch <= 'z')
        return (ch - 'a') + 'A';
    return ch;
}
static void gen_short_name_impl(const char* long_name, char* name)
{
    // memcpy(name, long_name, 8);
    while(*long_name == ' ')
        long_name++;
    uint8_t short_i = 0;
    size_t i = 0;
    for (; short_i < 8 && i < strlen(long_name); i++)
    {
        if (long_name[i] == ' ')
            continue;
        if (long_name[i] == '.')
            break;
        name[short_i++] = toupper(long_name[i]);
    }
    for (; short_i < 8; short_i++)
        name[short_i] = ' ';
    if (strrfind(long_name, '.') != SIZE_MAX)
    {
        size_t off = strrfind(long_name, '.')+1;
        for (uint8_t i = 0; i < 3 && i < strlen(long_name+off); i++)
            name[i+8] = toupper(long_name[off+i]);
    }
}
static void gen_short_name(const char* long_name, char* name, fat_dirent_cache* parent, fat_dirent_cache* dirent)
{
    memset(name, 0, 11);
    if (strlen(long_name) <= 11)
    {
        // memcpy(name, long_name, 11);
        gen_short_name_impl(long_name, name);
        return;
    }
    uint32_t n = 1;
    char raw[11] = {};
    gen_short_name_impl(long_name, raw);
    for (fat_dirent_cache* curr = parent->fdc_children.head; curr; )
    {
        if (curr == dirent)
        {
            curr = curr->fdc_next_child;
            continue;
        }
        char curr_name[11] = {};
        gen_short_name_impl(OBOS_GetStringCPtr(&curr->name), curr_name);
        if (memcmp(raw, curr_name, 11))
            n++;
        curr = curr->fdc_next_child;
    }
    OBOS_ASSERT(n <= 999999);
    uint8_t basename_len = 0;
    for (; raw[basename_len] != ' ' && basename_len < 8; basename_len++)
        ;
    size_t sz = snprintf(nullptr, 0, "%d", n);
    raw[basename_len-sz-1] = '~';
    char buf[7] = {};
    snprintf(buf, sz+1, "%d", n);
    memcpy(&raw[basename_len-sz], buf, sz);
    memcpy(name, raw, 11);
}
static const char* basename(const char* path)
{
    size_t base = strrfind(path, '/');
    if (base == SIZE_MAX)
        return path;
    return path + base + 1;
}
#define DIRENT_FREE (char)0xe5
static void deref_dirent(fat_dirent_cache* cache_entry)
{
    fat_cache* cache = cache_entry->owner;
    int64_t clusterSize = cache->blkSize*cache->bpb->sectorsPerCluster;
    off_t sector_offset = cache_entry->dirent_offset;
    // Vfs_FdSeek(cache->volume, cache_entry->dirent_fileoff, SEEK_SET);
    // Vfs_FdRead(cache->volume, sector, clusterSize, nullptr);
    OBOS_ASSERT(clusterSize < OBOS_PAGE_SIZE);
    uint8_t* sector = VfsH_PageCacheGetEntry(cache->vn, cache_entry->dirent_fileoff, true);
    fat_dirent* curr = (fat_dirent*)(sector + cache_entry->dirent_offset);
    curr--;
    while (curr->attribs & LFN)
    {
        curr->filename_83[0] = DIRENT_FREE;
        curr--;
        sector_offset -= sizeof(fat_dirent);
        if (sector_offset == 0)
        {
            // // TODO: Follow cluster chain.
            // sector_offset = clusterSize - sizeof(fat_dirent);
            // Vfs_FdSeek(cache->volume, -clusterSize, SEEK_CUR);
            // Vfs_FdWrite(cache->volume, sector, clusterSize, nullptr);
            // Vfs_FdSeek(cache->volume, -clusterSize, SEEK_CUR);
            // Vfs_FdRead(cache->volume, sector, clusterSize, nullptr);
            // wroteback = true;
            // curr = (fat_dirent*)(sector + cache_entry->dirent_offset);
            // curr--;
            // lmao just let someone else free the orphaned lfn entries
            break; 
        }
    }
    cache_entry->data.filename_83[0] = DIRENT_FREE;
    memcpy(sector + cache_entry->dirent_offset, &cache_entry->data, sizeof(cache_entry->data));
    // WriteFatDirent(cache, cache_entry);
}
static uint8_t checksum(const char *pFcbName)
{
    uint16_t FcbNameLen;
    uint8_t Sum;
    Sum = 0;
    for (FcbNameLen=11; FcbNameLen!=0; FcbNameLen--)
        Sum = ((Sum & 1) ? 0x80 : 0) + (Sum >> 1) + *pFcbName++;
    return Sum;
}
static void process_dirent(fat_cache* cache, const fat_dirent* curr, fat_dirent_cache* parent, uint32_t off, uint32_t* const fileoff, uint32_t* const nFree)
{
    if (curr->filename_83[0] == DIRENT_FREE)
    {
        if (!(*nFree)++)
            *fileoff = off;
    }
    else 
    {
        *fileoff = 0;
        *nFree = 0;
    }
    OBOS_UNUSED(cache);
    OBOS_UNUSED(parent);
    OBOS_UNUSED(parent);
}
iterate_decision find_free_entry(uint32_t cluster, obos_status status, void* userdata)
{
    if (status == OBOS_STATUS_ABORTED)
        return ITERATE_DECISION_STOP;
    fat_cache* cache = (void*)((uintptr_t*)userdata)[0];
    fat_dirent_cache* parent = (void*)((uintptr_t*)userdata)[1];
    uint32_t nEntries = ((uintptr_t*)userdata)[2];
    // void* buff = (void*)((uintptr_t*)userdata)[3];
    uint32_t* nFree = (void*)((uintptr_t*)userdata)[4];
    uint32_t* fileoff = (void*)((uintptr_t*)userdata)[5];
    (*(size_t*)((uintptr_t*)userdata)[6])++;
    uint32_t* entry_cluster = (void*)((uintptr_t*)userdata)[7];
    const size_t clusterSize = cache->blkSize*cache->bpb->sectorsPerCluster;
    // Vfs_FdSeek(cache->volume, ClusterToSector(cache, cluster)*cache->blkSize, SEEK_SET);
    // Vfs_FdRead(cache->volume, buff, clusterSize, nullptr);
    const void* buff = VfsH_PageCacheGetEntry(cache->vn, ClusterToSector(cache, cluster)*cache->blkSize, false);
    const fat_dirent* curr = buff;
    for (size_t j = 0; j < (clusterSize/sizeof(fat_dirent)); j++)
    {
        if (curr->filename_83[0] == (char)0)
        {
            // TODO: Account for the other clusters.
            *entry_cluster = cluster;
            *nFree = (clusterSize/sizeof(fat_dirent))-j;
            *fileoff = ClusterToSector(cache, cluster)*cache->blkSize+(j*sizeof(fat_dirent));
            break;
        }
        process_dirent(cache, curr, parent, ClusterToSector(cache, cluster)*cache->blkSize+(j*sizeof(fat_dirent)), fileoff, nFree);
        if ((*nFree) == 1)
            *entry_cluster = cluster;
        if ((*nFree) >= nEntries)
        {
            curr = nullptr;
            break;
        }
        curr++;
    }
    return ITERATE_DECISION_CONTINUE;
}
static void ref_dirent(fat_dirent_cache* cache_entry)
{
    fat_cache* cache = cache_entry->owner;
    lfn_dirent* lfnEntries = nullptr;
    uint8_t nLfnEntries = 0;
    if (OBOS_GetStringSize(&cache_entry->name) > 11)
    {
        // we need to generate lfn entries.
        nLfnEntries = (OBOS_GetStringSize(&cache_entry->name) / 13) + ((OBOS_GetStringSize(&cache_entry->name) % 13) != 0);
        lfnEntries = FATAllocator->ZeroAllocate(FATAllocator, nLfnEntries, sizeof(lfn_dirent), nullptr);
        size_t nBytesLeft = OBOS_GetStringSize(&cache_entry->name);
        uint8_t chksum = checksum((char*)&cache_entry->data.filename_83);
        for (uint8_t order = 0; order < nLfnEntries; order++)
        {
            lfnEntries[order].order = order+1;
            lfnEntries[order].checksum = chksum;
            lfnEntries[order].mustBeZero = 0;
            if (order == (nLfnEntries - 1))
                lfnEntries[order].order |= 0x40;
            lfnEntries[order].attrib |= LFN;
            for (size_t i = 0, limit = nBytesLeft >= 13 ? 13 : nBytesLeft; i < limit; i++)
            {
                if (i < 5)
                    lfnEntries[order].name1[i] = OBOS_GetStringCPtr(&cache_entry->name)[i];
                else if (i >= 5 && i < 11)
                    lfnEntries[order].name2[i - 5] = OBOS_GetStringCPtr(&cache_entry->name)[i];
                else if (i == 11 || i == 12)
                    lfnEntries[order].name3[i - 11] = OBOS_GetStringCPtr(&cache_entry->name)[i];
            }
        }
    }
    size_t nEntries = nLfnEntries + 1;
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    size_t nClusters = nEntries*sizeof(fat_dirent);
    nClusters /= bytesPerCluster;
    if ((nEntries*sizeof(fat_dirent)) % bytesPerCluster)
        nClusters++;
    // Search for enough free entries
    bool inRoot = cache->root == cache_entry->fdc_parent;
    uint32_t nFree = 0;
    uint32_t fileoff = 0;
    uint32_t entry_cluster = 0;
    size_t szClusters = 0;
    size_t blkSize = 0;
    if (inRoot && cache->fatType != FAT32_VOLUME)
    {
        // We need to iterate over each sector in the root directory.
        blkSize = cache->blkSize;
        // void* buff = FATAllocator->ZeroAllocate(FATAllocator, 1, cache->blkSize, nullptr);
        Vfs_FdSeek(cache->volume, cache->root_sector*cache->blkSize, SEEK_SET);
        for (size_t i = cache->root_sector; i < (cache->root_sector+cache->RootDirSectors); i++)
        {
            void* buff = VfsH_PageCacheGetEntry(cache->volume->vn, i*cache->blkSize, false);
            fat_dirent* curr = buff;
            for (size_t j = 0; j < (cache->blkSize/sizeof(fat_dirent)); j++)
            {
                if (curr->filename_83[0] == (char)0)
                {
                    nFree = (cache->blkSize/sizeof(fat_dirent))-j;
                    // fnuy business
                    nFree += (cache->RootDirSectors-(i-cache->root_sector))*(cache->blkSize/sizeof(fat_dirent));
                    fileoff = (i*cache->blkSize)+(j*sizeof(fat_dirent));
                    curr = nullptr;
                    break;
                }
                process_dirent(cache, curr, cache_entry->fdc_parent, (i*cache->blkSize)+(j*sizeof(fat_dirent)), &fileoff, &nFree);
                if (nFree >= nEntries)
                {
                    curr = nullptr;
                    break;
                }
                curr++;
            }
            if (!curr)
                break;
        }
    }
    else 
    {
        // We need to iterate over each cluster in the directory.
        blkSize = bytesPerCluster;
        uintptr_t udata[] = {
            (uintptr_t)cache,
            (uintptr_t)cache_entry,
            nEntries,
            (uintptr_t)0,
            (uintptr_t)&nFree,
            (uintptr_t)&fileoff,
            (uintptr_t)&szClusters,
            (uintptr_t)&entry_cluster
        };
        uint32_t first_cluster = cache_entry->fdc_parent->data.first_cluster_low;
        first_cluster |= ((uint32_t)cache_entry->fdc_parent->data.first_cluster_high << 16);
        FollowClusterChain(cache, first_cluster, find_free_entry, udata);
    }
    // void *buf = FATAllocator->ZeroAllocate(FATAllocator, 1, bytesPerCluster, nullptr);
    if (nFree < nEntries)
    {
        // We need MOREEEEEEEEEEEE clusters
        if (inRoot && cache->fatType != FAT32_VOLUME)
            return; // nothing we can do about this...
        size_t newSizeCls = szClusters + nClusters;
        uint32_t cluster = cache_entry->fdc_parent->data.first_cluster_low;
        cluster |= ((uint32_t)cache_entry->fdc_parent->data.first_cluster_high << 16);
        if (!ExtendClusters(cache, cluster, newSizeCls, szClusters))
        {
            uint32_t newCluster = AllocateClusters(cache, newSizeCls);
            if (newCluster == UINT32_MAX)
            {
                Core_MutexRelease(&cache->fat_lock);
                return;
            }
            for (size_t i = 0; i < szClusters; i++)
            {
                const void* buf = VfsH_PageCacheGetEntry(cache->vn, ClusterToSector(cache, cluster+i)*cache->blkSize, false);
                void* out = VfsH_PageCacheGetEntry(cache->vn, ClusterToSector(cache, newCluster+i)*cache->blkSize, true);
                memcpy(out, buf, bytesPerCluster);
            }
            if (cluster)
                FreeClusters(cache, cluster, szClusters);
            cluster = newCluster;
            cache_entry->fdc_parent->data.first_cluster_high = cluster >> 16;
            cache_entry->fdc_parent->data.first_cluster_low = cluster & 0xffff;
            if (cache_entry != cache->root)
                WriteFatDirent(cache, cache_entry->fdc_parent, false);
            else 
            {
                cache->bpb->ebpb.fat32.rootCluster = cluster;
                WriteFatDirent(cache, cache_entry->fdc_parent, false);
                Vfs_FdSeek(cache->volume, 0, SEEK_SET);
                // memzero(buf, cache->blkSize);
                // memcpy(buf, cache->bpb, sizeof(*cache->bpb));
                // Vfs_FdWrite(cache->volume, buf, cache->blkSize, nullptr);
                memcpy(VfsH_PageCacheGetEntry(cache->vn, 0, true), cache->bpb, sizeof(*cache->bpb));
            }
        }
        fileoff = ClusterToSector(cache, cluster)*cache->blkSize;
        entry_cluster = cluster;
    }
    Vfs_FdSeek(cache->volume, fileoff, SEEK_SET);
    void* buf = VfsH_PageCacheGetEntry(cache->vn, fileoff, false);
    fileoff = Vfs_FdTellOff(cache->volume);
    fat_dirent* curr = buf+(fileoff%blkSize);
    for (size_t i = 0; i < nEntries; i++)
    {
        fat_dirent* curr_dirent = &cache_entry->data;
        if (i < nLfnEntries)
            curr_dirent = (fat_dirent*)&lfnEntries[i];
        memcpy(curr, curr_dirent, sizeof(*curr));
        curr++;
        if ((uintptr_t)curr >= ((uintptr_t)buf+blkSize))
        {
            if (inRoot && cache->fatType != FAT32_VOLUME)
            {
                // Simply read the next sector.
                Vfs_FdSeek(cache->volume, fileoff, SEEK_SET);
                fileoff = Vfs_FdTellOff(cache->volume);
                buf = VfsH_PageCacheGetEntry(cache->vn, fileoff, true);
                curr = buf;
            }
            else
            {
                // Read the next cluster.
                Vfs_FdSeek(cache->volume, fileoff, SEEK_SET);
                uint32_t next = 0;
                fat_entry_addr addr = {};
                GetFatEntryAddrForCluster(cache, entry_cluster, &addr);
                uint8_t* sector = VfsH_PageCacheGetEntry(cache->vn, addr.lba*cache->blkSize, false);
                obos_status status = NextCluster(cache, entry_cluster, sector, &next);
                if (status == OBOS_STATUS_EOF)
                    break;
                if (next == 0)
                {
                    OBOS_Error("FAT: Error following cluster chain: Unexpected free cluster. Aborting.\n");
                    FATAllocator->Free(FATAllocator, buf, bytesPerCluster);
                    break;
                }
                if (next >= cache->CountofClusters)
                {
                    OBOS_Error("FAT: Error following cluster chain: Cluster is over disk boundaries. Aborting.\n");
                    FATAllocator->Free(FATAllocator, buf, bytesPerCluster);
                    break;
                }
                Vfs_FdSeek(cache->volume, ClusterToSector(cache, next)*cache->blkSize, SEEK_SET);
                fileoff = Vfs_FdTellOff(cache->volume);
                Vfs_FdRead(cache->volume, buf, blkSize, nullptr);
                buf = VfsH_PageCacheGetEntry(cache->vn, fileoff/cache->blkSize*cache->blkSize, true);
                curr = buf;
            }
        }
    }
}
obos_status move_desc_to(dev_desc desc, const char* where)
{
    if (!desc || !where)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!valid_filename(where, true))
        return OBOS_STATUS_INVALID_ARGUMENT;
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)desc;
    fat_cache* cache = cache_entry->owner;
    /*
    To move a entry:
        - IF the paths are the same
            - return SUCCESS
        - IF the new path already exists
            - return ALREADY_EXISTS
        - IF the new parent doesn't exist
            - return NOT_FOUND
        - Change it's name to the new name
        - Change it's path (and all its childrens' path) to the new path
        - IF the paths differ, and the parent directory of both paths differ
            - Dereference the cache node from its parent, and add it to the new parent.
            - Dereference the dirent from its parent, and add it to the new parent
        - else
            - Dereference the dirent from its parent, and add it with the new basename
        - exit
    */
    do {
        fat_dirent_cache* found = DirentLookupFrom(where, cache->root);
        if (found == cache_entry)
            return OBOS_STATUS_SUCCESS;
        if (found)
            return OBOS_STATUS_ALREADY_INITIALIZED;
    } while(0);
    string parent_path = {};
    OBOS_InitStringLen(&parent_path, where, strrfind(where, '/') == SIZE_MAX ? strlen(where) : strrfind(where, '/'));
    fat_dirent_cache* parent = DirentLookupFrom(OBOS_GetStringCPtr(&parent_path), cache->root);
    if (!parent && OBOS_GetStringSize(&parent_path) == strlen(where))
    {
        parent = cache->root;
        parent_path = cache->root->path;
    }
    if (!parent)
        return OBOS_STATUS_NOT_FOUND;
    OBOS_FreeString(&cache_entry->name);
    OBOS_FreeString(&cache_entry->path);
    cache_entry->path = parent_path;
    if (OBOS_GetStringSize(&cache_entry->path))
        if (OBOS_GetStringCPtr(&cache_entry->path)[OBOS_GetStringSize(&cache_entry->path) - 1] != '/')
            OBOS_AppendStringC(&cache_entry->path, "/");
    OBOS_InitString(&cache_entry->name, basename(where));
    OBOS_AppendStringS(&cache_entry->path, &cache_entry->name);
    if (parent != cache_entry->fdc_parent)
    {
        Core_MutexAcquire(&cache->fat_lock);
        Core_MutexAcquire(&cache->fd_lock);
        deref_dirent(cache_entry);
        CacheRemoveChild(cache_entry->fdc_parent, cache_entry);
        CacheAppendChild(parent, cache_entry);
        gen_short_name(OBOS_GetStringCPtr(&cache_entry->name), &cache_entry->data.filename_83[0], parent, cache_entry);
        ref_dirent(cache_entry);
        Core_MutexRelease(&cache->fd_lock);
        Core_MutexRelease(&cache->fat_lock);
    }
    else
    {
        deref_dirent(cache_entry);
        Core_MutexAcquire(&cache->fat_lock);
        Core_MutexAcquire(&cache->fd_lock);
        gen_short_name(OBOS_GetStringCPtr(&cache_entry->name), &cache_entry->data.filename_83[0], parent, cache_entry);
        ref_dirent(cache_entry);
        Core_MutexRelease(&cache->fd_lock);
        Core_MutexRelease(&cache->fat_lock);
    }
    Vfs_FdFlush(cache->volume);
    return OBOS_STATUS_SUCCESS;
}
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
    deref_dirent(cache_entry);
    const size_t bytesPerCluster = (cache->bpb->sectorsPerCluster*cache->blkSize);
    const uint32_t szClusters = ((cache_entry->data.filesize / bytesPerCluster) + ((cache_entry->data.filesize % bytesPerCluster) != 0));
    FreeClusters(cache, (uint32_t)cache_entry->data.first_cluster_low|((uint32_t)cache_entry->data.first_cluster_high<<16), szClusters);
    CacheRemoveChild(cache_entry->fdc_parent, cache_entry);
    cache->fileCount--;
    FATAllocator->Free(FATAllocator, cache_entry, sizeof(*cache_entry));
    Vfs_FdFlush(cache->volume);
    return OBOS_STATUS_SUCCESS;
}
