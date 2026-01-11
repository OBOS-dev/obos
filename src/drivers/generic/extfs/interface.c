/*
 * drivers/generic/extfs/interface.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <mm/page.h>
#include <mm/swap.h>

#include <allocators/base.h>

#include <locks/mutex.h>

#include <driver_interface/header.h>

#include "structs.h"

#define get_handle(desc) ({\
    if (!desc) return OBOS_STATUS_INVALID_ARGUMENT;\
    (ext_inode_handle*)desc;\
})

obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    ext_inode_handle* hnd = get_handle(desc);
    Core_MutexAcquire(&hnd->lock);
    page* pg = nullptr;
    ext_inode* ino = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);
    MmH_RefPage(pg);
    
    uint32_t new_mode = ino->mode & ~0777;

    if (newperm.other_exec)
        new_mode |= EXT_OTHER_EXEC;
    if (newperm.owner_exec)
        new_mode |= EXT_OWNER_EXEC;
    if (newperm.group_exec)
        new_mode |= EXT_GROUP_EXEC;

    if (newperm.other_write)
        new_mode |= EXT_OTHER_WRITE;
    if (newperm.owner_write)
        new_mode |= EXT_OWNER_WRITE;
    if (newperm.group_write)
        new_mode |= EXT_GROUP_WRITE;
    
    if (newperm.other_read)
        new_mode |= EXT_OTHER_READ;
    if (newperm.owner_read)
        new_mode |= EXT_OWNER_READ;
    if (newperm.group_read)
        new_mode |= EXT_GROUP_READ;

    ino->mode = new_mode;
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    Core_MutexRelease(&hnd->lock);
    return OBOS_STATUS_SUCCESS;
}

obos_status set_file_owner(dev_desc desc, uid owner_uid, gid group_uid)
{
    ext_inode_handle* hnd = get_handle(desc);
    Core_MutexAcquire(&hnd->lock);
    page* pg = nullptr;
    ext_inode* ino = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);
    OBOS_ASSERT(ino);
    if (!ino)
        return OBOS_STATUS_INTERNAL_ERROR;
    MmH_RefPage(pg);
    
    if (owner_uid != -1)
        ino->uid = owner_uid & 0xffff;
    if (group_uid != -1)
        ino->gid = group_uid & 0xffff;
    
    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    Core_MutexRelease(&hnd->lock);
    return OBOS_STATUS_SUCCESS;
}

obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    ext_inode_handle* hnd = (void*)desc;
    if (!hnd || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc == UINTPTR_MAX)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ext_inode* node = ext_read_inode(hnd->cache, hnd->ino);
    if (!node)
        return OBOS_STATUS_INVALID_ARGUMENT; // uh oh :D
    *count = node->size;
    Free(EXT_Allocator, node, sizeof(*node));
    return OBOS_STATUS_SUCCESS;    
}

obos_status stat_fs_info(void *vn, drv_fs_info *info)
{
    if (!info)
        return OBOS_STATUS_INVALID_ARGUMENT;

    ext_cache *cache = nullptr;
    for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
    {
        if (curr->vn == vn)
            cache = curr;

        curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
    }

    if (!cache)
        return OBOS_STATUS_NOT_FOUND;

    info->fsBlockSize = cache->block_size;
    info->freeBlocks = le32_to_host(cache->superblock.block_count) - le32_to_host(cache->superblock.free_block_count);

    info->availableFiles = le32_to_host(cache->superblock.free_inode_count);
    info->fileCount = le32_to_host(cache->superblock.inode_count) - le32_to_host(cache->superblock.free_inode_count);

    info->nameMax = 255;

    if (cache->read_only)
        info->flags |= FS_FLAGS_RDONLY;

    info->partBlockSize = cache->vn->blkSize;
    info->szFs = cache->vn->filesize / info->partBlockSize;

    return OBOS_STATUS_SUCCESS;
}


obos_status set_file_times(dev_desc desc, void* times_)
{
    struct file_times* times = times_;
    ext_inode_handle* hnd = (void*)desc;

    if (!hnd || !times)
        return OBOS_STATUS_INVALID_ARGUMENT;

    page* pg = nullptr;
    Core_MutexAcquire(&hnd->lock);

    ext_inode* ino = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);

    ino->access_time = times->access;
    ino->creation_time = times->birth;
    ino->modification_time = times->change;
    ino->delete_time = 0;

    Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    
    Core_MutexRelease(&hnd->lock);

    return OBOS_STATUS_SUCCESS;
}

static dev_desc get_desc(ext_cache* cache, uint32_t ino)
{
    if (cache->inode_vnode_table[ino-1])
        return cache->inode_vnode_table[ino-1]->desc;
    else
    {
        ext_inode_handle* hnd = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_inode_handle), nullptr);
        hnd->cache = cache;
        hnd->ino = ino;
        hnd->lock = MUTEX_INITIALIZE();
        return (dev_desc)hnd;
    }
}

obos_status vnode_search(void** vn_found, dev_desc desc, void* dev_vn)
{
    uint32_t ino = 0;
    ext_cache* cache = nullptr;
    if (desc == UINTPTR_MAX)
    {
        for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
        {
            if (curr->vn == dev_vn)
                cache = curr;
    
            curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
        }
        if (!cache)
            return OBOS_STATUS_INVALID_ARGUMENT;
        ino = 2;
    }
    else 
    {
        ext_inode_handle* hnd = (void*)desc;
        cache = hnd->cache;
        ino = hnd->ino;
        OBOS_ENSURE(cache);
    }
    
    *vn_found = ext_make_vnode(cache, ino, nullptr);
    return OBOS_STATUS_SUCCESS;
}

obos_status list_dir(dev_desc dir, void* vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata)
{
    Mm_PageWriterOperation |= PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);

    uint32_t ino = 0;
    ext_cache* cache = nullptr;
    if (dir == UINTPTR_MAX)
    {
        for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
        {
            if (curr->vn == vn)
                cache = curr;
    
            curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
        }
        if (!cache)
            return OBOS_STATUS_INVALID_ARGUMENT;
        ino = 2;
    }
    else 
    {
        ext_inode_handle* hnd = (void*)dir;
        cache = hnd->cache;
        ino = hnd->ino;
    }

    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    MmH_RefPage(pg);
    if (!ext_ino_test_type(inode, EXT2_S_IFDIR))
    {
        MmH_DerefPage(pg);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }

    size_t nToRead = le32_to_host(inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, ino, 0, nToRead, buffer, nullptr);
    
    ext_dirent* ent = nullptr;
    char name[256] = {};
    for (size_t offset = 0; offset < nToRead; )
    {
        ent = (void*)(buffer+offset);
        if (!ent->ino)
            goto down;
        if (strcmp(ent->name, ".") || strcmp(ent->name, ".."))
            goto down;

        ext_inode* ent_ino = ext_read_inode(cache, ent->ino);
        OBOS_ASSERT(ent_ino);
        if (!ent_ino)
            goto down;

        memcpy(name, ent->name, ent->name_len);
        if (cb(get_desc(cache, ent->ino), 1, ext_ino_filesize(cache, ent_ino), userdata, name) == ITERATE_DECISION_STOP)
        {
            Free(EXT_Allocator, ent_ino, sizeof(ext_inode));
            break;
        }
        memzero(name, ent->name_len);
        
        Free(EXT_Allocator, ent_ino, sizeof(ext_inode));
        
        down:
        offset += ent->rec_len;
        if (!ent->rec_len)
        {
            // Corrupted directory.
            MmH_DerefPage(pg);
            return OBOS_STATUS_INTERNAL_ERROR;
        }
    }

    MmH_DerefPage(pg);
    inode = nullptr;

    return OBOS_STATUS_SUCCESS;
}

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}

#define get_next_tok() do {\
    const char *newtok = tok + str_search(tok, '/');\
    tok = newtok;\
    size_t currentPathLen = strlen(tok);\
    if (currentPathLen)\
        currentPathLen--;\
    else { tok_len = 0; break; }\
    if (tok[currentPathLen] != '/')\
        currentPathLen++;\
    while (tok[currentPathLen] == '/')\
        currentPathLen--;\
    tok_len = strchr(tok, '/');\
    if (tok_len != currentPathLen)\
        tok_len--;\
    while (tok[tok_len - 1] == '/')\
        tok_len--;\
} while(0)

static void on_match(ext_inode** const inode, 
                     page** const pg,
                     uint8_t** const buffer,
                     size_t* const nToRead,
                     size_t* const offset,
                     const ext_dirent* const dent,
                     ext_cache* const cache)
{
    uint32_t ino = dent->ino;
    if (dent->file_type == EXT2_FT_DIR)
    {
        size_t old_size = *nToRead;
        *nToRead = le32_to_host((*inode)->blocks) * 512;
        *buffer = Reallocate(EXT_Allocator, *buffer, *nToRead, old_size, nullptr);
        ext_ino_read_blocks(cache, ino, 0, *nToRead, *buffer, nullptr);
        *offset = 0;
    }
    MmH_DerefPage(*pg);
    *inode = ext_read_inode_pg(cache, ino, pg);
    MmH_RefPage(*pg);
}

obos_status path_search(dev_desc* found, void* vn, const char* path, dev_desc parent)
{
    if (!found || (!vn && parent == UINTPTR_MAX) || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;

    Mm_PageWriterOperation |= PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);        

    uint32_t parent_ino  = 0;
    ext_cache* cache = nullptr;
    if (parent == UINTPTR_MAX)
    {
        for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
        {
            if (curr->vn == vn)
                cache = curr;

            curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
        }
        if (!cache)
            return OBOS_STATUS_INVALID_ARGUMENT;
        
        parent_ino = 2;
    }
    else
    {
        ext_inode_handle* hnd = (void*)parent;
        if (!parent)
            return OBOS_STATUS_INVALID_ARGUMENT;
        cache = hnd->cache;
        parent_ino = hnd->ino;
    }

    *found = 0;

    size_t path_len = strlen(path);
    for (; *path == '/'; path++, path_len--)
        ;
    if (!path_len)
        return OBOS_STATUS_NOT_FOUND;
    const char* tok = path;
    size_t tok_len = strchr(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    while (tok[tok_len - 1] == '/')
        tok_len--;
    if (!tok_len)
        return OBOS_STATUS_NOT_FOUND;

    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, parent_ino, &pg);
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    MmH_RefPage(pg);
    if (!ext_ino_test_type(inode, EXT2_S_IFDIR))
    {
        MmH_DerefPage(pg);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }

    size_t nToRead = le32_to_host(inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, parent_ino, 0, nToRead, buffer, nullptr);
    
    ext_dirent* ent = nullptr;
    for (size_t offset = 0; offset < nToRead; )
    {
        ent = (void*)(buffer+offset);
        if (!ent->ino)
            goto down;
        if (strcmp(ent->name, ".") || strcmp(ent->name, ".."))
            goto down;

        ext_inode* ent_ino = ext_read_inode(cache, ent->ino);

        if (ent->name_len == tok_len)
        {
            if (memcmp(ent->name, tok, tok_len))
            {
                get_next_tok();
                if (tok == (path + path_len))
                {
                    *found = get_desc(cache, ent->ino);
                    break;
                }
                on_match(&inode,
                         &pg,
                         &buffer,
                         &nToRead,
                         &offset,
                         ent,
                         cache);
            }
        }
        
        Free(EXT_Allocator, ent_ino, sizeof(ext_inode));

        down:
        offset += ent->rec_len;
        if (!ent->rec_len)
        {
            // Directory corruption.
            OBOS_Error("extfs: %s: directory corrupted, returning OBOS_STATUS_INTERNAL_ERROR (EIO)\n", __func__);
            MmH_DerefPage(pg);
            Free(EXT_Allocator, buffer, nToRead);
            return OBOS_STATUS_INTERNAL_ERROR;
        }
    }

    MmH_DerefPage(pg);
    Free(EXT_Allocator, buffer, nToRead);

    return *found ? OBOS_STATUS_SUCCESS : OBOS_STATUS_NOT_FOUND;
}