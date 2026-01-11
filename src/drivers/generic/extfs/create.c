/*
 * drivers/generic/extfs/create.c
 *
 * Copyright (c) 2026 Omar Berrow
 *
 * Abandon all hope, ye who enter here.
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/base.h>

#include <mm/page.h>

#include <mm/swap.h>

#include "structs.h"

static obos_status expand_directory(ext_cache* cache, ext_dirent_cache* parent, size_t* new_ent_offset, size_t* prev_ent_offset)
{
    size_t block = 0, old_size = ext_ino_filesize(cache, parent->inode);

    obos_status status = ext_ino_resize(cache, parent->ent.ino, old_size + block, true);
    if (obos_is_error(status))
        return status;
    status = ext_ino_commit_blocks(cache, parent->ent.ino, old_size, block);
    if (obos_is_error(status))
        return status;

    size_t nToRead = le32_to_host(parent->inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, parent->ent.ino, 0, nToRead, buffer, nullptr);

    ext_dirent* iter = nullptr;
    size_t offset = 0;
    for (; offset < old_size; )
    {
        iter = (void*)(buffer+offset);
        offset += iter->rec_len;
    }

    *prev_ent_offset = offset - iter->rec_len;
    // the last entry should already point to the new entry.
    *new_ent_offset = old_size;

    return OBOS_STATUS_SUCCESS;
}

static obos_status make_dirent(
    ext_dirent_cache** out, 
    ext_cache* cache,
    ext_dirent_cache* parent,
    const char* name,
    uint32_t ino, bool ino_new,
    file_type type)
{
    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(cache, ino, &pg);
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    MmH_RefPage(pg);
    if (ino_new)
        memzero(inode, sizeof(*inode));

    // look for space for the dirent

    size_t nToRead = le32_to_host(parent->inode->blocks) * 512;
    uint8_t* buffer = Allocate(EXT_Allocator, nToRead, nullptr);
    ext_ino_read_blocks(cache, parent->ent.ino, 0, nToRead, buffer, nullptr);
    
    size_t hole_begin = 0, hole_end = 0;
    uint16_t ent_len = strlen(name) + (sizeof(ext_dirent)-255);
    size_t ent_offset = 0, prev_ent_offset = 0;
    
    ext_dirent* iter = nullptr;
    size_t offset = 0;
    for (; offset < nToRead; )
    {
        iter = (void*)(buffer+offset);
        
        if (!hole_begin)
        {
            size_t real_begin = offset + (sizeof(ext_dirent)-255) + iter->name_len;
            if (!iter->ino)
                real_begin = offset;
            hole_begin = real_begin;
            hole_begin += 3;
            hole_begin &= ~3;
            if ((hole_begin - real_begin) > (iter->rec_len-real_begin))
            {
                hole_begin = 0;
                goto next;
            }
            prev_ent_offset = offset;
        }
        else
        {
            hole_end = offset;
            size_t hole_size = hole_end - hole_begin;
            if (hole_size >= ent_len)
            {
                ent_offset = hole_begin;
                ent_len += (hole_size-ent_len);
                break;
            }
            hole_end = 0;
            prev_ent_offset = offset;
            size_t real_begin = offset + (sizeof(ext_dirent)-255) + iter->name_len;
            if (!iter->ino)
                real_begin = offset;
            hole_begin = real_begin;
            hole_begin += 3;
            hole_begin &= ~3;
        }
        
        next:
        offset += iter->rec_len;
    }

    Free(OBOS_KernelAllocator, buffer, nToRead);
    buffer = nullptr;

    if (!hole_begin)
    {
        obos_status status = expand_directory(cache, parent, &ent_offset, &prev_ent_offset);
        if (obos_is_error(status))
            return status;
    }

    if (hole_begin && !hole_end)
    {
        hole_end = offset;
        size_t hole_size = hole_end - hole_begin;
        if (hole_size < ent_len)
        {
            obos_status status = expand_directory(cache, parent, &ent_offset, &prev_ent_offset);
            if (obos_is_error(status))
                return status;
        }
        else
        {
            ent_offset = hole_begin;
            ent_len += (hole_size-ent_len);
        }
    }

    ext_dirent_cache* ent = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_dirent_cache), nullptr);
    ent->rel_offset = ent_offset;
    ent->ent_block = ext_get_block_at_index(cache, parent->ent.ino, ext_get_blk_index_from_offset(cache, ent_offset));
    OBOS_ENSURE(ent->ent_block != 0);
    ent->ent_offset = ent_offset % cache->block_size;
    ent->parent = parent;
    ent->inode = inode;
    ent->pg = pg;
    ent->ent.ino = ino;
    ent->ent.rec_len = ent_len;
    ent->ent.name_len = strlen(name);
    memcpy(ent->ent.name, name, ent->ent.name_len);

    ext_dirent_cache* emplace_at = nullptr;
    bool found = false;
    for (ext_dirent_cache* node = parent->children.head; node; node = node->next)
    {
        if (node->rel_offset == prev_ent_offset)
        {
            // We have closed the hole, change the rec_len of the previous entry.
            node->ent.rec_len = ent_offset - prev_ent_offset;
            ext_dirent_flush(cache, node);
            if (node->next)
                emplace_at = node;
            found = true;
            break;
        }
    }

    OBOS_ENSURE(found);
    
    if (emplace_at)
        ext_dirent_emplace_at(parent, ent, emplace_at);
    else
        ext_dirent_adopt(parent, ent);

    if (cache->superblock.revision > 0)
    {
        switch (type) {
            case FILE_TYPE_DIRECTORY:
                ent->ent.file_type = EXT2_FT_DIR;
                break;
            case FILE_TYPE_REGULAR_FILE:
                ent->ent.file_type = EXT2_FT_REG_FILE;
                break;
            case FILE_TYPE_SYMBOLIC_LINK:
                ent->ent.file_type = EXT2_FT_SYMLINK;
                break;
            default: OBOS_UNREACHABLE;
        }
    }
    
    ext_dirent_flush(cache, ent);

    *out = ent;
    return OBOS_STATUS_SUCCESS;
}

static void directory_finalize(ext_cache* cache, ext_inode* inode, uint32_t ino, uint32_t parent_ino)
{
    const size_t dir_size = cache->block_size * 4;
    ext_dirent* ents = Allocate(EXT_Allocator, dir_size, nullptr);
    ext_dirent* end = (ext_dirent*)(((uintptr_t)ents) + dir_size);
    inode->size = dir_size;
#if ext_sb_supports_64bit_filesize
    inode->dir_acl = (dir_size >> 32);
#endif
    inode->blocks = dir_size / 512;
    obos_status status = ext_ino_commit_blocks(cache, ino, 0, inode->size);
    OBOS_ASSERT(obos_is_success(status));
    if (obos_is_error(status))
        return;
    ext_dirent* dot = ents;
    dot->rec_len = (sizeof(ext_dirent)-255) + 4;
    dot->name_len = 1;
    dot->name[0] = '.';
    if (cache->revision > 0)
        dot->file_type = EXT2_FT_DIR;
    dot->ino = ino;
    ext_dirent* dotdot = (ext_dirent*)(((uintptr_t)dot) + dot->rec_len);
    size_t rec_len = dir_size - ((sizeof(ext_dirent)-255) + 4);
    if (rec_len > cache->block_size)
    {
        ext_dirent* current_entry = dotdot;
        rec_len += (cache->block_size-1);
        rec_len &= ~(cache->block_size-1);
        while (rec_len != 0 && (current_entry < end))
        {
            memzero(current_entry, sizeof(*current_entry));
            if (current_entry == dotdot)
                current_entry->rec_len = cache->block_size - ((sizeof(ext_dirent)-255) + 4);
            else
                current_entry->rec_len = cache->block_size;
            current_entry = (ext_dirent*)(((uintptr_t)current_entry) + current_entry->rec_len);
            rec_len -= cache->block_size;
        }
        current_entry->rec_len = cache->block_size;
    }
    else
        dotdot->rec_len = rec_len;
    dotdot->name_len = 2;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    if (cache->revision > 0)
        dotdot->file_type = EXT2_FT_DIR;
    dotdot->ino = parent_ino;
    inode->link_count++;
    do {
        page* pg = nullptr;
        ext_inode* parent = ext_read_inode_pg(cache, parent_ino, &pg);
        parent->link_count++;
    } while(0);
    
    cache->bgdt[ext_ino_get_block_group(cache, ino)].used_directories++;
    ext_writeback_bgd(cache, ext_ino_get_block_group(cache, ino));
    ext_ino_write_blocks(cache, ino, 0, dir_size, ents, nullptr);

    Free(EXT_Allocator, ents, dir_size);
}

obos_status pmk_file(dev_desc* newDesc, 
    const char* parent_path,
    void* vn,
    const char* name,
    file_type type,
    driver_file_perm perm)
{
    if (!newDesc || !parent_path || !vn || !name || (type > FILE_TYPE_SYMBOLIC_LINK))
        return OBOS_STATUS_INVALID_ARGUMENT;

    Mm_PageWriterOperation |= PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);

    ext_cache *cache = nullptr;
    for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
    {
        if (curr->vn == vn)
            cache = curr;

        curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
    }

    if (!cache)
        return OBOS_STATUS_NOT_FOUND;

    if (cache->read_only)
        return OBOS_STATUS_READ_ONLY;

    if ((strlen(name)) > 255)
        return OBOS_STATUS_INVALID_ARGUMENT; // TODO ENAMETOOLONG
    if (name[strchr(name, '/')] == '/')
        return OBOS_STATUS_INVALID_ARGUMENT;

    ext_dirent_cache* parent = ext_dirent_lookup_from(parent_path, cache->root);
    if (!parent)
        return OBOS_STATUS_NOT_FOUND;
    if (parent != cache->root)
    {
        char name[256] = {};
        memcpy(name, parent->ent.name, parent->ent.name_len);
        ext_dirent_populate(cache, parent->ent.ino, name, false, parent);
    }
    
    ext_dirent_cache* ent = nullptr;

    uint32_t newino = ext_ino_allocate(cache, nullptr);
    if (!newino)
        return OBOS_STATUS_NO_SPACE;

    obos_status status = make_dirent(&ent, cache, parent, name, newino, true, type);
    if (obos_is_error(status))
    {
        ext_ino_free(cache, newino);
        return status;
    }

    if (cache->revision == 1)
        ent->inode->dir_acl = 0;
    ent->inode->size = 0;
    ent->inode->gid = ROOT_GID;
    ent->inode->uid = ROOT_UID;
    if (perm.set_uid)
        ent->inode->mode |= EXT_SETUID;
    if (perm.set_gid)
        ent->inode->mode |= EXT_SETGID;
    if (perm.owner_read)
        ent->inode->mode |= EXT_OWNER_READ;
    if (perm.owner_write)
        ent->inode->mode |= EXT_OWNER_WRITE;
    if (perm.owner_exec)
        ent->inode->mode |= EXT_OWNER_EXEC;
    if (perm.group_read)
        ent->inode->mode |= EXT_GROUP_READ;
    if (perm.group_write)
        ent->inode->mode |= EXT_GROUP_WRITE;
    if (perm.group_exec)
        ent->inode->mode |= EXT_GROUP_EXEC;
    if (perm.other_read)
        ent->inode->mode |= EXT_OTHER_READ;
    if (perm.other_write)
        ent->inode->mode |= EXT_OTHER_WRITE;
    if (perm.other_exec)
        ent->inode->mode |= EXT_OTHER_EXEC;
    ent->inode->link_count++;
    
    switch (type)
    {
        case FILE_TYPE_SYMBOLIC_LINK:
            ent->inode->mode |= EXT2_S_IFLNK;
            break;
        case FILE_TYPE_DIRECTORY:
            ent->inode->mode |= EXT2_S_IFDIR;
            directory_finalize(cache, ent->inode, newino, parent->ent.ino);
            break;
        case FILE_TYPE_REGULAR_FILE:
            ent->inode->mode |= EXT2_S_IFREG;
            break;
        default:
            OBOS_ENSURE(!"unimplemented");
    }

    Mm_MarkAsDirtyPhys(ent->pg);

    ext_inode_handle* hnd = ZeroAllocate(EXT_Allocator, 1, sizeof(*hnd), nullptr);
    hnd->cache = cache;
    hnd->ino = newino;
    hnd->lock = MUTEX_INITIALIZE();
    *newDesc = (dev_desc)hnd;

    return OBOS_STATUS_SUCCESS;
}


obos_status phardlink_file(dev_desc desc, const char* parent_path, void* vn, const char* name)
{
    ext_inode_handle* to_link = (void*)desc;

    Mm_PageWriterOperation |= PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);

    if (!parent_path || !to_link || !name)
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

    if (cache != to_link->cache)
        return OBOS_STATUS_ACCESS_DENIED;

    ext_dirent_cache* parent = ext_dirent_lookup_from(parent_path, cache->root);
    if (!parent)
        return OBOS_STATUS_NOT_FOUND;
    if (parent != cache->root)
    {
        char name[256] = {};
        memcpy(name, parent->ent.name, parent->ent.name_len);
        ext_dirent_populate(cache, parent->ent.ino, name, false, parent);
    }
    
    ext_dirent_cache* ent = nullptr;

    file_type type = 0;
    ext_inode* ino = ext_read_inode(cache, to_link->ino);
    if (ext_ino_test_type(ino, EXT2_S_IFDIR))
    {
        Free(EXT_Allocator, ino, sizeof(*ino));
        return OBOS_STATUS_ACCESS_DENIED;
    }
    else if (ext_ino_test_type(ino, EXT2_S_IFREG))
        type = FILE_TYPE_REGULAR_FILE;
    else if (ext_ino_test_type(ino, EXT2_S_IFLNK))
        type = FILE_TYPE_SYMBOLIC_LINK;
    else
    {
        Free(EXT_Allocator, ino, sizeof(*ino));
        return OBOS_STATUS_UNIMPLEMENTED;
    }

    Free(EXT_Allocator, ino, sizeof(*ino));

    obos_status status = make_dirent(&ent, cache, parent, name, to_link->ino, false, type);
    if (obos_is_error(status))
        return status;

    ent->inode->link_count++;
    Mm_MarkAsDirtyPhys(ent->pg);

    return OBOS_STATUS_SUCCESS;
}

obos_status symlink_set_path(dev_desc desc, const char* to)
{
    ext_inode_handle* hnd = (void*)desc;
    if (!hnd)
        return OBOS_STATUS_INVALID_ARGUMENT;

    page* pg = nullptr;
    ext_inode* inode = ext_read_inode_pg(hnd->cache, hnd->ino, &pg);
    OBOS_ASSERT(inode);
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    MmH_RefPage(pg);
    
    if (!ext_ino_test_type(inode, EXT2_S_IFLNK))
    {
        MmH_DerefPage(pg);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }

    bool dirty = false;
    size_t path_len = strlen(to);
    bool fast_symlink = path_len <= 60;

    if (fast_symlink)
        memcpy((char*)&inode->direct_blocks, to, path_len);
    else
    {
        obos_status status = ext_ino_resize(hnd->cache, hnd->ino, path_len, false);
        if (obos_is_error(status))
        {
            MmH_DerefPage(pg);
            return status;
        }

        status = ext_ino_commit_blocks(hnd->cache, hnd->ino, 0, path_len);
        if (obos_is_error(status))
        {
            MmH_DerefPage(pg);
            return status;
        }
        
        status = ext_ino_write_blocks(hnd->cache, hnd->ino, 0, path_len, to, nullptr);
        if (obos_is_error(status))
        {
            MmH_DerefPage(pg);
            return status;
        }
    }
    
    inode->size = path_len;

    if (dirty)
        Mm_MarkAsDirtyPhys(pg);
    MmH_DerefPage(pg);
    
    return OBOS_STATUS_SUCCESS;
}

obos_status premove_file(void* vn, const char* path)
{
    ext_cache *cache = nullptr;
    for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
    {
        if (curr->vn == vn)
            cache = curr;

        curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
    }

    if (!cache)
        return OBOS_STATUS_NOT_FOUND;
    
    ext_dirent_cache* dent = ext_dirent_lookup_from(path, cache->root);
    if (!dent)
        return OBOS_STATUS_NOT_FOUND;
    if (strncmp(dent->ent.name, ".", dent->ent.name_len) || strncmp(dent->ent.name, "..", dent->ent.name_len))
        return OBOS_STATUS_ACCESS_DENIED;

    ext_dirent_cache* prev = dent->prev;
    ext_dirent_cache* next = dent->next;
    if (prev)
    {
        prev->ent.rec_len = (next ? next->rel_offset : dent->parent->inode->blocks*512) - prev->rel_offset;
        ext_dirent_flush(cache, prev);
    }
    
    dent->ent.ino = 0;
    dent->ent.file_type = 0;
    ext_dirent_flush(cache, dent);

    ext_dirent_disown(dent->parent, dent);
    if (!(--dent->inode->link_count))
        ext_ino_free(cache, dent->ent.ino);
    MmH_DerefPage(dent->pg);

    Free(EXT_Allocator, dent, sizeof(*dent));
    
    return OBOS_STATUS_SUCCESS;    
}