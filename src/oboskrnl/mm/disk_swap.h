/*
 * oboskrnl/mm/disk_swap.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

// Defines the interface for setting up a swap device to swap out to disk (vnodes)

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>
#include <partition.h>

#include <mm/swap.h>

enum {
    DISK_SWAP_MAGIC = 0xAD537B31,
};

enum {
    // TODO: Support.
    DISK_SWAP_FLAGS_HIBERNATE = BIT(0),
};

// In little-endian

typedef struct disk_swap_node {
    uint64_t next_lba;
    size_t nPages;
} disk_swap_node;

#define DISK_SWAP_VERSION (1U)
typedef struct disk_swap_header
{
    uint32_t magic;
    uint32_t flags;
    uint64_t reserved_block_count; // block count - reserved block count = possible block count
    uint32_t version;
    char pad[]; // padding bytes = block_size - sizeof(disk_swap_header)
} OBOS_PACK disk_swap_header;

obos_status Mm_MakeDiskSwap(partition* part);
obos_status Mm_InitializeDiskSwap(swap_dev* dev, partition* part);