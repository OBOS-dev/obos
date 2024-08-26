/*
 * oboskrnl/mbr.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>
#include <partition.h>

#include <vfs/fd.h>

typedef enum mbr_partition_type
{
    MBR_PARTITION_FAT12,
    MBR_PARTITION_XENIX_ROOT,
    MBR_PARTITION_XENIX_USR,
    MBR_PARTITION_FAT16,
    MBR_PARTITION_FAT16B = 0x6,
    MBR_PARTITION_IFS = 0x7,
    MBR_PARTITION_HPFS = 0x7,
    MBR_PARTITION_NTFS = 0x7,
    MBR_PARTITION_exFAT = 0x7,
    MBR_PARTITION_FAT32_CHS = 0xb,
    MBR_PARTITION_FAT32,
    MBR_PARTITION_FAT16B_LBA,
} mbr_partition_type;
typedef struct mbr_partition
{
    uint8_t  status;
    uint8_t  chs_start[3];
    uint8_t  type;
    uint8_t  chs_end[3];
    uint32_t lba;
    uint32_t nSectors;
} OBOS_PACK mbr_partition;
typedef struct mbr
{
    uint8_t boot_sector[446];
    mbr_partition parts[4];
    uint16_t signature; // 0xAA55
} OBOS_PACK mbr_t;
OBOS_STATIC_ASSERT(sizeof(mbr_t) == 512, "sizeof(mbr_t) is not 512 bytes!");
OBOS_STATIC_ASSERT(sizeof(mbr_partition) == 16, "sizeof(mbr_partition) is not 16 bytes!");
#define MBR_BOOT_SIGNATURE 0xAA55
obos_status OBOS_IdentifyMBRPartitions(fd* desc, partition* partition_list, size_t* nPartitions);