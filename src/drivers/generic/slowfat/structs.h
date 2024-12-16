/*
 * drivers/generic/slowfat/structs.h
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * Abandon all hope ye who enter here
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <allocators/base.h>

#include <locks/mutex.h>

#include <utils/list.h>
#include <utils/string.h>

typedef struct fsinfo
{
    uint32_t leadSignature; // 0x41615252
    char resv1[480];
    uint32_t other_signature; // 0x61417272
    uint32_t lastFreeCluster;
    uint32_t firstAvailableCluster; // If 0xffffffff, start at two
    char resv2[12];
    uint32_t trailSignature; // 0xAA550000
} OBOS_PACK fsinfo;
typedef struct ebpb32 {
    uint32_t fatSz32;
    uint16_t extendedFlags;
    uint16_t fsVersion;
    uint32_t rootCluster;
    uint16_t fsInfoOffset;
    uint16_t bkBootSector;
    char resv1[12];
    uint8_t driveNumber;
    char resv2[1];
    uint8_t bootSignature;
    uint32_t volumeId;
    char volumeLabel[11];
    char doNotUse[8];
} OBOS_PACK ebpb32;
typedef struct ebpb {
    uint8_t driveNumber;
    uint8_t reversed;
    uint8_t bootSignature; // 0x29
    uint32_t volumeId;
    char volumeLabel[11];
    char doNotUse[8];
} OBOS_PACK ebpb;
typedef struct bpb {
    char jmpboot[3];
    char oem_name[8];
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t nFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t media;
    uint16_t fatSz16;
    uint16_t sectorsPerTrack;
    uint16_t nHeads;
    uint32_t nHiddenSectors;
    uint32_t totalSectors32;
    union {
        ebpb32 fat32;
        ebpb fat;
    } ebpb;
} OBOS_PACK bpb;
enum {
    READ_ONLY=0x01,
    HIDDEN=0x02,
    SYSTEM=0x04, 
    VOLUME_ID=0x08,
    DIRECTORY=0x10,
    ARCHIVE=0x20,
    LFN=READ_ONLY|HIDDEN|SYSTEM|VOLUME_ID,
};
typedef struct fat_date
{
    uint8_t      day : 5;
    uint8_t    month : 4;
    uint8_t year1980 : 7;
} OBOS_PACK fat_date;
typedef struct fat_time
{
    uint8_t seconds : 5; // multiply by two
    uint8_t minutes : 6;
    uint8_t hour : 5;
} OBOS_PACK fat_time;
typedef struct fat_dirent
{
    char filename_83[11];
    uint8_t attribs;
    uint8_t resv;
    uint8_t unused; // creation time in hundredths of a second. unused, since it's pretty obscure, and useless to us
    fat_time creation_time;
    fat_date creation_date;
    fat_date access_date;
    uint16_t first_cluster_high; // only valid on FAT32
    fat_time last_mod_time;
    fat_date last_mod_date;
    uint16_t first_cluster_low;
    uint32_t filesize;
} OBOS_PACK fat_dirent;
// Mustn't exceed 255 characters.
typedef struct lfn_dirent
{
    // if bit 6 is set, this is the last entry. this is always set in the first lfn entry of a set
    uint8_t order;
    uint16_t name1[5];
    uint8_t attrib; // must be LFN
    uint8_t type; // TODO: what is this?
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t mustBeZero; // fstClusLO
    uint16_t name3[2];
} OBOS_PACK lfn_dirent;
OBOS_STATIC_ASSERT(sizeof(fat_dirent) == 32, "sizeof(fat_dirent) isn't 32 bytes.");
OBOS_STATIC_ASSERT(sizeof(lfn_dirent) == 32, "sizeof(lfn_dirent) isn't 32 bytes.");
typedef struct fat_dirent_cache
{
    fat_dirent data;
    string name;
    string path;
    // The offset in bytes of the cluster or sector this dirent is on.
    uint64_t dirent_fileoff;
    // The offset into the dirent sector in which this dirent is at.
    uint32_t dirent_offset;
    struct fat_cache* owner;
    struct
    {
        struct fat_dirent_cache* parent;
        struct
        {
            struct fat_dirent_cache* head;
            struct fat_dirent_cache* tail;
            size_t nChildren;
        } children;
        struct fat_dirent_cache* next_child;
        struct fat_dirent_cache* prev_child;
    } tree_info;
} fat_dirent_cache;
#define fdc_children tree_info.children
#define fdc_next_child tree_info.next_child
#define fdc_prev_child tree_info.prev_child
#define fdc_parent tree_info.parent
enum {
    FAT32_VOLUME,
    FAT16_VOLUME,
    FAT12_VOLUME,
};
typedef LIST_HEAD(fat_cache_list, struct fat_cache) fat_cache_list;
LIST_PROTOTYPE(fat_cache_list, struct fat_cache, node);
typedef struct fat_freenode
{
    uint32_t cluster;
    uint32_t nClusters;
    struct fat_freenode *next, *prev;
} fat_freenode;
typedef struct fat_cache {
    fat_dirent_cache* root;
    uint8_t fatType;
    bpb* bpb;
    struct fd* volume;
    mutex fd_lock;
    struct vnode* vn;
    LIST_NODE(fat_cache_list, struct fat_cache) node;
    uint32_t FirstDataSector;
    uint32_t RootDirSectors;
    uint32_t root_cluster;
    uint64_t root_sector;
    uint32_t CountofClusters;
    size_t blkSize;
    uint32_t fatSz;
    mutex fat_lock;
    struct {
        fat_freenode *head, *tail;
        size_t nNodes;
        size_t freeClusterCount;
        mutex lock;
    } freelist;
} fat_cache;
extern fat_cache_list FATVolumes;
void CacheAppendChild(fat_dirent_cache* parent, fat_dirent_cache* child);
void CacheRemoveChild(fat_dirent_cache* parent, fat_dirent_cache* what);
typedef struct fat_entry_addr
{
    uint64_t lba;
    uint32_t offset;
} fat_entry_addr;
typedef struct {
    uint32_t ent : 28; 
} OBOS_ALIGN(4) fat32_entry;
typedef struct {
    uint16_t ent; 
} OBOS_ALIGN(2) fat16_entry;
// FAT12 is funny.
typedef struct {
    uint16_t ent : 12; 
} OBOS_ALIGN(2) fat12_entry;
void GetFatEntryAddrForCluster(fat_cache* cache, uint32_t cluster, fat_entry_addr* out);
uint32_t GetClusterFromFatEntryAddr(fat_cache* cache, fat_entry_addr addr);
fat12_entry GetFat12Entry(uint16_t val, uint32_t valCluster);
fat_dirent_cache* DirentLookupFrom(const char* path, fat_dirent_cache* root);
#define ClusterToSector(cache, n) (((n) - 2) * (cache)->bpb->sectorsPerCluster + (cache)->FirstDataSector)
#define SectorToCluster(cache, n) (((int64_t)(n)-(int64_t)(cache)->FirstDataSector+(int64_t)(cache)->bpb->sectorsPerCluster*2)/(int64_t)(cache)->bpb->sectorsPerCluster)
//extern allocator_info* FATAllocator;
#define FATAllocator OBOS_KernelAllocator
obos_status WriteFatDirent(fat_cache* cache, fat_dirent_cache* cache_entry, bool lock);
