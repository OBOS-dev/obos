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

#include <mm/swap.h>

typedef struct obos_swap_free_region
{
    // In bytes from the beginning of the files.
    uint64_t next, prev;
    // MUST be < hdr.size
    uint64_t size; // size including sizeof_this
    uint64_t sizeof_this;
    // The region starts right after this header.
    // If the device being used is a block device, then it will be on the next block.
} OBOS_PACK obos_swap_free_region;
typedef struct obos_swap_header
{
    // Must be from 128-sector size. 
    // If the device is not a block device, it must be 128
    uint64_t header_size; 
    // Must be filesize - header_size
    uint64_t size; 
    struct {
        uint64_t head; // head in byte offset from the start of the vnode 
        uint64_t tail; // tail in byte offset from the start of the vnode
        uint64_t nNodes; // the amount of free nodes.
        uint64_t freeBytes; // the amount of free bytes
    } OBOS_PACK freelist;
    uint32_t flags;
    uint32_t header_version; // current version is OBOS_SWAP_HEADER_VERSION
    uint32_t magic; // must be OBOS_SWAP_HEADER_MAGIC
    uint8_t resv[68]; // must be zero
} OBOS_PACK obos_swap_header;
OBOS_STATIC_ASSERT(sizeof(obos_swap_header) == 128, "Invalid swap header size!");
OBOS_STATIC_ASSERT(sizeof(obos_swap_free_region) == 32, "Invalid swap free region size!");
enum {
    OBOS_SWAP_HEADER_VERSION = 1,
    OBOS_SWAP_HEADER_MAGIC_x86_64 = 0x50A9DE61,
    OBOS_SWAP_HEADER_MAGIC_m68k = 0x50A9DE62,
#if defined (__x86_64__)
    OBOS_SWAP_HEADER_MAGIC = OBOS_SWAP_HEADER_MAGIC_x86_64,
#elif defined (__m68k__)
    OBOS_SWAP_HEADER_MAGIC = OBOS_SWAP_HEADER_MAGIC_m68k,
#endif
};
enum {
    // If set, the system didn't properly stop the swap device (possibly due to a power failure/forced shutdown).
    // If that is the case, then the system must reinitailize the freelist.
    OBOS_SWAP_HEADER_DIRTY = 0b1,
};

obos_status MmH_InitializeDiskSwapDevice(swap_dev *dev, void* vnode);
obos_status MmH_InitializeDiskSwap(void* vnode);