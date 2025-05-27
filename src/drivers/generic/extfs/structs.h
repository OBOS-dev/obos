/*
 * drivers/generic/extfs/structs.h
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#include <int.h>
#include <struct_packing.h>

#include <vfs/pagecache.h>

#include <mm/page.h>

#if defined(__x86_64__)
#   define host_to_be16(val) __builtin_bswap16(val)
#   define host_to_be32(val) __builtin_bswap32(val)
#   define host_to_be64(val) __builtin_bswap64(val)
#   define be16_to_host(val) __builtin_bswap16(val)
#   define be32_to_host(val) __builtin_bswap32(val)
#   define be64_to_host(val) __builtin_bswap64(val)
#   define host_to_le16(val) (uint16_t)(val)
#   define host_to_le32(val) (uint32_t)(val)
#   define host_to_le64(val) (uint32_t)(val)
#   define le16_to_host(val) (uint16_t)(val)
#   define le32_to_host(val) (uint32_t)(val)
#   define le64_to_host(val) (uint64_t)(val)
#elif defined(__m68k__)
#   define host_to_be16(val) (uint16_t)(val)
#   define host_to_be32(val) (uint32_t)(val)
#   define host_to_be64(val) (uint32_t)(val)
#   define be16_to_host(val) (uint16_t)(val)
#   define be32_to_host(val) (uint32_t)(val)
#   define be64_to_host(val) (uint64_t)(val)
#   define host_to_le16(val) __builtin_bswap16(val)
#   define host_to_le32(val) __builtin_bswap32(val)
#   define host_to_le64(val) __builtin_bswap64(val)
#   define le16_to_host(val) __builtin_bswap16(val)
#   define le32_to_host(val) __builtin_bswap32(val)
#   define le64_to_host(val) __builtin_bswap64(val)
#else
#   error Define required macros.
#endif

enum {
    EXT_MAGIC = 0xEF53,
};
enum {
    EXT2_FEATURE_INCOMPAT_COMPRESSION = BIT(0),
    EXT2_FEATURE_INCOMPAT_FILETYPE = BIT(1),
    EXT3_FEATURE_INCOMPAT_RECOVER = BIT(2),
    EXT3_FEATURE_INCOMPAT_JOURNAL_DEV = BIT(3),
    EXT2_FEATURE_INCOMPAT_META_BG = BIT(4),
};
enum {
    EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER = BIT(0),
    EXT2_FEATURE_RO_COMPAT_LARGE_FILE = BIT(1),
    EXT2_FEATURE_RO_COMPAT_BTREE_DIR = BIT(2),
};
enum {
    EXT_VALID_FS = 1,
    EXT_ERROR_FS = 2,
};
enum {
    EXT_ERROR_CONTINUE,
    EXT_ERROR_RO, // remount as RO
    EXT_ERROR_PANIC, // kernel panic
};
enum {
    EXT_GOOD_OLD_REV = 0,
    EXT_DYNAMIC_REV = 1,
};

typedef struct ext_superblock
{
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t resv_block_count;
    uint32_t free_block_count;
    uint32_t free_inode_count;
    uint32_t first_data_block;
    uint32_t log_block_size; // 1024<<log_block_size = block size
    uint32_t log_fragment_size; // ignored
    uint32_t blocks_per_group;
    uint32_t fragments_per_group;
    uint32_t inodes_per_group;
    uint32_t mount_time;
    uint32_t write_time;
    uint16_t mount_count; // how many times the fs was mounted since last verification
    uint16_t max_mount_count; // how many times the fs can be mounted before verification
    uint16_t magic;
    uint16_t state;
    uint16_t error_behavior;
    uint16_t minor_rev;
    uint32_t last_check;
    uint32_t check_interval;
    uint32_t creator_os;
    uint32_t revision;
    uint16_t default_resv_uid;
    uint16_t default_resv_gid;
    struct {
        uint32_t first_ino;
        uint16_t inode_size; // must be a power of two (popcount(inode_size) == 1)
        uint16_t block_group;
        uint32_t features;
        uint32_t incompat_features; // if any of these features are unsupported, refuse to mount
        uint32_t ro_only_features; // if any of these features are unsupported, mount as RO
        uint8_t unused_uuid[16];
        char volume_name[16];
        char last_path_mounted[64];
        uint32_t bitmap_algorithm;
    } OBOS_PACK dynamic_rev;
    uint8_t padding[1024-204];
} OBOS_PACK ext_superblock;

// Block Group Descriptor
typedef struct ext_bgd {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks;
    uint16_t free_inodes;
    uint16_t used_directories;
    uint16_t padding;
    uint8_t resv[12];
} OBOS_PACK ext_bgd, *ext_bgdt;

enum {
    EXT2_BAD_INO = 1,
    EXT2_ROOT_INO,
    EXT2_ACL_IDX_INO,
    EXT2_ACL_DATA_INO,
    EXT2_BOOT_LOADER_INO,
    EXT2_UNDEL_DIR_INO,
};

#define EXEC BIT(0)
#define WRITE BIT(1)
#define READ BIT(2)

enum {
    EXT_OTHER_EXEC = EXEC<<0,
    EXT_OTHER_WRITE = WRITE<<0,
    EXT_OTHER_READ = READ<<0,
    EXT_GROUP_EXEC = EXEC<<3,
    EXT_GROUP_WRITE = WRITE<<3,
    EXT_GROUP_READ = READ<<3,
    EXT_OWNER_EXEC = EXEC<<6,
    EXT_OWNER_WRITE = WRITE<<6,
    EXT_OWNER_READ = READ<<6,
    EXT_STICKY_BIT = BIT(9),
    EXT_SETGID = BIT(10),
    EXT_SETUID = BIT(11),
    EXT2_S_IFIFO = 0x1000,
    EXT2_S_IFCHR = 0x2000,
    EXT2_S_IFDIR = 0x4000,
    EXT2_S_IFBLK = 0x6000,
    EXT2_S_IFREG = 0x8000,
    EXT2_S_IFLNK = 0xA000,
    EXT2_S_IFSOCK = 0xC000,
};

#undef EXEC
#undef WRITE
#undef READ

typedef struct ext_inode { 
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t access_time;
    uint32_t creation_time;
    uint32_t modification_time;
    uint32_t delete_time;
    uint16_t gid;
    uint16_t link_count; // does not include soft links, acts as a refcount
    uint32_t blocks; // in 512-byte blocks, not ext blocks
    uint32_t flags;
    uint32_t os1;
    uint32_t direct_blocks[12];
    uint32_t indirect_block;
    uint32_t doubly_indirect_block;
    uint32_t triply_indirect_block;
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl; // revision one, contains the top 32-bits of the file size for regular files
    uint32_t fragment;
    uint8_t os2[12];
} OBOS_PACK ext_inode;

typedef struct ext_dirent {
    uint32_t ino;
    uint16_t rec_len;
    uint8_t name_len;
    uint8_t file_type; // ignore, use inode->mode. also invalid in revision 0
    char name[]; // 255 bytes max
} OBOS_PACK ext_dirent;

typedef struct ext_dirent_cache {
    uint32_t ino;
    uint32_t block;
    struct {
        struct ext_dirent_cache *head, *tail;
        size_t nChildren;
    } children;
    struct ext_dirent_cache *next, *prev;
    ext_dirent ent;
} ext_dirent_cache;

#define ext_dirent_adopt(parent_, child_) \
do {\
    ext_dirent_cache* _parent = (parent_);\
    ext_dirent_cache* _child = (child_);\
    if(!_parent->children.head)\
        _parent->children.head = child;\
    if (_parent->children.tail)\
        _parent->children.tail->next = child;\
    child->prev = _parent->children.tail;\
    _parent->children.tail = child;\
    _parent->children.nChildren++;\
} while(0)

#define ext_dirent_disown(parent_, child_) \
do {\
    ext_dirent_cache* _parent = (parent_);\
    ext_dirent_cache* _child = (child_);\
    if (_parent->children.head == _child)\
        _parent->children.head = _child->next;\
    if (_parent->children.tail == _child)\
        _parent->children.tail = _child->prev;\
    if (_child->prev->next)\
        _child->prev->next = _child->next;\
    if (_child->next->prev)\
        _child->next->prev = _child->prev;\
    _parent->children.nChildren--;\
} while(0)

typedef struct ext_cache {
    ext_superblock superblock;
    vnode* vn;
    ext_bgdt bgdt;
    uint32_t block_size;
    uint32_t revision;
    uint32_t block_group_count;
    uint32_t inode_blocks_per_group;
    uint32_t inodes_per_block;
    uint16_t inodes_per_group;
    uint16_t blocks_per_group;
    uint16_t inode_size;
} ext_cache;

// Gets an inode straight from the pagecache, returns it along the page* of the pagecache entry.
ext_inode* ext_read_inode_pg(ext_cache* cache, uint32_t ino, page **pg);
// Wrapper for ext_read_inode_pg that copies it into a separate buffer and returns it.
ext_inode* ext_read_inode(ext_cache* cache, uint32_t ino);

#define ext_read_block(cache, block_number, pg) (VfsH_PageCacheGetEntry((cache)->vn, (block_number)*(cache->block_size), (pg)))
#define ext_block_group_from_block(cache, block_number) ((block_number) / (cache)->blocks_per_group)

#define ext_ino_filesize(cache, inode) (le32_to_host((inode)->size) | ((cache)->revision > 1 ? le32_to_host((inode)->dir_acl) : 0))
#define ext_ino_max_block_index(cache, inode) (le32_to_host(inode->blocks) / ((cache->block_size) / 512))
#define ext_ino_get_block_group(cache, inode_number) ((inode_number - 1) / (cache)->inodes_per_group)
#define ext_ino_get_local_index(cahe, inode_number) ((inode_number - 1) % (cache)->inodes_per_group)

#if OBOS_ARCHITECTURE_BITS == 64
#define ext_sb_supports_64bit_filesize (true)
#else
#define ext_sb_supports_64bit_filesize (false)
#endif
#define ext_sb_block_size(superblock) (1024<<le32_to_host((superblock)->log_block_size))
#define ext_sb_blocks_per_group(superblock) (le16_to_host((superblock)->blocks_per_group))
#define ext_sb_inodes_per_group(superblock) (le16_to_host((superblock)->inodes_per_group))
#define ext_sb_inode_size(superblock) (le32_to_host((superblock)->revision) == EXT_DYNAMIC_REV ? le16_to_host((superblock)->dynamic_rev.inode_size) : 128)

#define EXT_Allocator OBOS_KernelAllocator