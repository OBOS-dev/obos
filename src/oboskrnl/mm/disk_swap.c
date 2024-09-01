/*
 * oboskrnl/mm/disk_swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <allocators/base.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <mm/swap.h>
#include <mm/disk_swap.h>

obos_status MmH_InitializeDiskSwapDevice(swap_dev *dev, void* vnode)
{
    OBOS_UNUSED(dev);
    OBOS_UNUSED(vnode);
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status MmH_InitializeDiskSwap(void* vn_)
{
    vnode* vn = vn_;
    size_t filesize = vn->filesize;
    if (filesize == sizeof(obos_swap_header))
        return OBOS_STATUS_INVALID_ARGUMENT;
    fd file = {};
    Vfs_FdOpenVnode(&file, vn, 0);
    size_t blkSize = Vfs_FdGetBlkSz(&file);
    obos_swap_header hdr = {
        .header_size=sizeof(obos_swap_header),
        .size = 0,
        .freelist = {},
        .flags = 0,
        .header_version = OBOS_SWAP_HEADER_VERSION,
        .magic = OBOS_SWAP_HEADER_MAGIC,
        .resv = {}
    };
    if (hdr.header_size % blkSize)
        hdr.header_size += (blkSize-(hdr.header_size%blkSize));
    hdr.size = filesize-hdr.header_size;
    obos_swap_free_region free = {};
    free.size = hdr.size - (hdr.size%blkSize);
    free.next = free.prev = 0;
    free.sizeof_this = sizeof(obos_swap_free_region);
    if (free.sizeof_this % blkSize)
        free.sizeof_this += (blkSize-(free.sizeof_this%blkSize));
    hdr.freelist.head = hdr.header_size;
    hdr.freelist.tail = hdr.header_size;
    hdr.freelist.nNodes++;
    uint8_t* buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 2, blkSize, nullptr);
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + blkSize, &free, sizeof(free));
    Vfs_FdWrite(&file, buf, blkSize*2, nullptr);
    Vfs_FdClose(&file);
    return OBOS_STATUS_SUCCESS;
}