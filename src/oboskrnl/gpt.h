/*
 * oboskrnl/gpt.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>
#include <partition.h>

#include <vfs/fd.h>

#define GPT_SIGNATURE 0x5452415020494645
typedef struct gpt_header
{
    uint64_t signature;
    uint32_t revision;
    uint32_t size; // must be 92...blksize
    uint32_t checksum; // CRC32 (without this field)
    uint32_t resv1;
    uint64_t this_lba;
    uint64_t alt_lba;
    uint64_t first_lba;
    uint64_t last_lba;
    char     disk_uuid[16];
    uint64_t part_table_lba;
    uint32_t part_entry_count;
    uint32_t sizeof_partiton_entry;
    uint32_t partition_entry_checksum;
    // rest of fields are reserved.
} OBOS_PACK gpt_header;
typedef enum gpt_partition_attrib
{
    GPT_ATTRIB_REQUIRED = BIT_TYPE(0, UL),
    GPT_NO_BLOCK_IO = BIT_TYPE(1, UL),
    GPT_LEGACY_BIOS_BOOTABLE = BIT_TYPE(2, UL),
    GPT_TYPE_UUID_START = BIT_TYPE(48, UL),
    GPT_TYPE_UUID_END = BIT_TYPE(63, UL),
} gpt_partition_attrib;
typedef struct gpt_partition_entry
{
    char     uuid[16];
    char     part_uuid[16];
    uint64_t begin_lba;
    uint64_t end_lba;
    uint64_t attributes;
    uint16_t part_name[36];
} OBOS_PACK gpt_partition_entry;
obos_status OBOS_IdentifyGPTPartitions(fd* desc, partition* partition_list, size_t* nPartitions, bool allow_checksum_fail);