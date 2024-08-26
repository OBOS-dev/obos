/*
 * drivers/generic/fat/structs.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

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
    fat_date last_mod_data;
    uint16_t first_cluster_low;
    uint32_t filesize;
} OBOS_PACK fat_dirent;
// Mustn't exceed 255 characters.
typedef struct lfn_dirent
{
    // if bit 6 is set, this is the last entry. this is always set in the first lfn entry of a set
    uint8_t order;
    char name1[10];
    uint8_t attrib; // must be LFN
    uint8_t type; // TODO: what is this?
    uint8_t checksum;
    char name2[12];
    uint16_t mustBeZero; // fstClusLO
    char name3[4];
} OBOS_PACK lfn_dirent;
OBOS_STATIC_ASSERT(sizeof(fat_dirent) == 32, "sizeof(fat_dirEntry) isn't 32 bytes.");
OBOS_STATIC_ASSERT(sizeof(lfn_dirent) == 32, "sizeof(lfn_dirent) isn't 32 bytes.");