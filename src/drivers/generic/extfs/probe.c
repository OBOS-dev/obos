/*
 * drivers/generic/extfs/probe.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/irp.h>
#include <vfs/alloc.h>

#include <allocators/base.h>

#include <mm/page.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <locks/mutex.h>

#include "structs.h"

bool probe(void* vn_)
{
    OBOS_ASSERT(vn_);
    if (!vn_)
        return false;
    vnode* vn = vn_;
    ext_cache* cache = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_cache), nullptr);
    cache->block_size = 1024;
    cache->vn = vn;
    page* pg = nullptr;
    ext_superblock* sb = ext_read_block(cache, 1, &pg);
    MmH_RefPage(pg);
    if (!sb)
    {
        MmH_DerefPage(pg);
        Free(EXT_Allocator, cache, sizeof(*cache));
        return false;
    }
    if (sb->magic != EXT_MAGIC)
    {
        MmH_DerefPage(pg);
        Free(EXT_Allocator, cache, sizeof(*cache));
        return false;
    }
    memcpy(&cache->superblock, sb, sizeof(*sb));
    MmH_DerefPage(pg);
    sb = &cache->superblock;
    pg = nullptr;
    cache->revision = le32_to_host(sb->revision);
    if (cache->revision)
    {
        uint32_t mask = EXT2_FEATURE_INCOMPAT_FILETYPE|EXT2_FEATURE_INCOMPAT_META_BG;
        bool incompatible = le32_to_host(sb->dynamic_rev.incompat_features) & ~mask;
        if (incompatible)
        {
            Free(EXT_Allocator, cache, sizeof(*cache));
            return false;
        }
        mask = (ext_sb_supports_64bit_filesize ? EXT2_FEATURE_RO_COMPAT_LARGE_FILE : 0) | EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER;
        cache->read_only = sb->dynamic_rev.ro_only_features & ~mask;
    }

    cache->block_size = ext_sb_block_size(sb);
    cache->blocks_per_group = ext_sb_blocks_per_group(sb);
    cache->inodes_per_group = ext_sb_inodes_per_group(sb);
    cache->inode_size = ext_sb_inode_size(sb);
    cache->block_group_count = le32_to_host(cache->superblock.block_count) / cache->blocks_per_group;
    cache->vn = vn;
    cache->inodes_per_block = cache->block_size/cache->inode_size;
    cache->inode_blocks_per_group = cache->inodes_per_group/cache->inodes_per_block;
    cache->bgdt = Allocate(EXT_Allocator, cache->block_group_count * sizeof(ext_bgd), nullptr);

    // Populate the in-memory BGDT
    do {
        uint32_t bgdt_blocks = (cache->block_group_count * sizeof(ext_bgd)) / cache->block_size;
        if ((cache->block_group_count * sizeof(ext_bgd)) % cache->block_size)
            bgdt_blocks++;
        for (size_t i = 0; i < bgdt_blocks; i++)
        {
            void* bgdt_section = ext_read_block(cache, (cache->block_size == 1024 ? 2 : 1) + i, &pg);
            MmH_RefPage(pg);
            size_t copy_count = i == (bgdt_blocks - 1) ? (cache->block_group_count % (cache->block_size / sizeof(ext_bgd))) : (cache->block_size / sizeof(ext_bgd));
            memcpy(cache->bgdt + (i*4096/sizeof(ext_bgd)), bgdt_section, copy_count*sizeof(ext_bgd));
            MmH_DerefPage(pg);
        }
    } while(0);

    OBOS_Debug("extfs: Block size: 0x%x\n", cache->block_size);
    OBOS_Debug("extfs: Blocks per group: 0x%x\n", cache->blocks_per_group);
    OBOS_Debug("extfs: Inodes per group: 0x%x\n", cache->inodes_per_group);
    OBOS_Debug("extfs: Inodes per block: 0x%x\n", cache->inodes_per_block);
    OBOS_Debug("extfs: Inode blocks per group: 0x%x\n", cache->inode_blocks_per_group);
    OBOS_Debug("extfs: Inode size: 0x%x\n", cache->inode_size);
    OBOS_Debug("extfs: Block group count: 0x%d\n", cache->block_group_count);
    OBOS_Debug("extfs: Revision: %d\n", cache->revision);

    if (cache->read_only)
        OBOS_Warning("extfs: Probed partition is read-only at probe. Likely due to unsupported ext features\n");

//    for (volatile bool b = true; b;)
//         ;

    ext_inode* root = ext_read_inode(cache, 2);
    if (!root)
    {
        OBOS_Error("extfs: No root inode in filesystem. Aborting probe\n");
        Free(EXT_Allocator, cache, sizeof(*cache));
        return false;
    }
    // We don't need the root inode except to check for its existence
    Free(EXT_Allocator, root, sizeof(*root));
    // cache->root = ext_dirent_populate(cache, 2, "/", true);

    cache->inode_vnode_table_size = cache->inodes_per_group*cache->block_group_count*sizeof(vnode*);
    cache->inode_vnode_table = Mm_VirtualMemoryAlloc(&Mm_KernelContext, 
                                                    nullptr, cache->inode_vnode_table_size, 
                                                    0, VMA_FLAGS_HUGE_PAGE,
                                                    nullptr, nullptr);

    LIST_APPEND(ext_cache_list, &EXT_CacheList, cache);

    return true;
}

vnode* ext_make_vnode(ext_cache* cache, uint32_t ino, mount* mnt)
{
    if (cache->inode_vnode_table[ino-1])
    {
        cache->inode_vnode_table[ino-1]->refs++;
        return cache->inode_vnode_table[ino-1];
    }

    ext_inode* inode = ext_read_inode(cache, ino);
    
    uint32_t vtype = 0;
    if (ext_ino_test_type(inode, EXT2_S_IFDIR))
        vtype = VNODE_TYPE_DIR;
    else if (ext_ino_test_type(inode, EXT2_S_IFREG))
        vtype = VNODE_TYPE_REG;
    else if (ext_ino_test_type(inode, EXT2_S_IFLNK))
        vtype = VNODE_TYPE_LNK;
    else
    {
        Free(EXT_Allocator, inode, sizeof(*inode));
        return nullptr;
    }

    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    cache->inode_vnode_table[ino-1] = vn;
    ext_inode_handle* handle = ZeroAllocate(EXT_Allocator, 1, sizeof(ext_inode_handle), nullptr);
    handle->ino = ino;
    handle->cache = cache;
    handle->lock = MUTEX_INITIALIZE();
    vn->desc = (dev_desc)handle;
    vn->vtype = vtype;
    vn->blkSize = 1;
    vn->owner_uid = inode->uid;
    vn->group_uid = inode->gid;
    vn->filesize = (uint64_t)inode->size | (ext_sb_supports_64bit_filesize ? (uint64_t)inode->dir_acl << 32 : 0);
    vn->times.access = inode->access_time;
    vn->times.birth = inode->creation_time;
    vn->times.access = inode->access_time;

    vn->perm.other_exec = inode->mode & EXT_OTHER_EXEC;
    vn->perm.other_write = inode->mode & EXT_OTHER_WRITE;
    vn->perm.other_read = inode->mode & EXT_OTHER_READ && !cache->read_only;
    
    vn->perm.owner_exec = inode->mode & EXT_OWNER_EXEC;
    vn->perm.owner_write = inode->mode & EXT_OWNER_WRITE;
    vn->perm.owner_read = inode->mode & EXT_OWNER_READ && !cache->read_only;

    vn->perm.group_exec = inode->mode & EXT_GROUP_EXEC;
    vn->perm.group_write = inode->mode & EXT_GROUP_WRITE;
    vn->perm.group_read = inode->mode & EXT_GROUP_READ && !cache->read_only;

    vn->perm.set_uid = inode->mode & EXT_SETUID;
    vn->perm.set_gid = inode->mode & EXT_SETGID;

    vn->mount_point = mnt;
    
    vn->inode = ino;

    if (vn->vtype == VNODE_TYPE_LNK)
        vn->un.linked = ext_ino_get_linked(cache, inode, ino);

    Free(EXT_Allocator, inode, sizeof(*inode));

    return vn;
}

static void mount_recursive(ext_cache* cache, ext_dirent_cache* parent, dirent* dparent, mount* mnt)
{
    OBOS_ENSURE(mnt);
    for (ext_dirent_cache* ent = parent->children.head; ent; )
    {
        vnode* vn = ext_make_vnode(cache, ent->ent.ino, mnt);
        if (!vn)
            goto down;

        dirent* dent = Vfs_Calloc(1, sizeof(dirent));
        OBOS_InitStringLen(&dent->name, ent->ent.name, ent->ent.name_len);
        dent->vnode = vn;
        VfsH_DirentAppendChild(dparent, dent);
        LIST_APPEND(dirent_list, &mnt->dirent_list, dent);
        dent->vnode->refs++;

        if (ent->ent.file_type == EXT2_FT_DIR)
            mount_recursive(cache, ent, dent, mnt);

        down:
        ent = ent->next;
    }
}

obos_status ext_mount(void* vn_, void* at_)
{
    if (!vn_ || !at_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    vnode* vn = vn_;
    dirent* at = at_;
    ext_cache *cache = nullptr;
    for (ext_cache* curr = LIST_GET_HEAD(ext_cache_list, &EXT_CacheList); curr && !cache; )
    {
        if (curr->vn == vn)
            cache = curr;

        curr = LIST_GET_NEXT(ext_cache_list, &EXT_CacheList, curr);
    }

    if (!cache)
        return OBOS_STATUS_NOT_FOUND;

    mount_recursive(cache, cache->root, at, at->vnode->un.mounted);

    return OBOS_STATUS_SUCCESS;
}
