/*
 * oboskrnl/mm/disk_swap.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <partition.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

#include <driver_interface/header.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

#include <irq/irql.h>

#include <mm/alloc.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/pmm.h>
#include <mm/disk_swap.h>

#include <utils/tree.h>

struct metadata
{
    vnode* vn;
    obos_swap_header hdr;
    driver_id* driver;
    size_t blkSize;
};

// static void* map(uintptr_t phys, size_t nPages, page** pages)
// {
//     void* virt = MmH_FindAvailableAddress(&Mm_KernelContext, nPages*OBOS_PAGE_SIZE, 0, nullptr);
//     page* buf = Mm_Allocator->ZeroAllocate(Mm_Allocator, nPages, sizeof(page), nullptr);
//     for (size_t i = 0; i < nPages; i++)
//     {
//         buf[i].owner = &Mm_KernelContext;
//         buf[i].pageable = false;
//         buf[i].prot.present = true;
//         buf[i].prot.rw = true;
//         buf[i].prot.huge_page = false;
//         buf[i].prot.ro = false;
//         buf[i].prot.uc = false;
//         buf[i].prot.user = false;
//         buf[i].addr = (uintptr_t)virt + i*OBOS_PAGE_SIZE;
//         MmS_SetPageMapping(Mm_KernelContext.pt, &buf[i], phys+i*OBOS_PAGE_SIZE);
//         RB_INSERT(page_tree, &Mm_KernelContext.pages, &buf[i]);
//     }
//     *pages = buf;
//     return virt;
// }
// static void unmap(size_t nPages, page* pages)
// {
//     for (size_t i = 0; i < nPages; i++)
//     {
//         pages[i].prot.present = false;
//         MmS_SetPageMapping(Mm_KernelContext.pt, &pages[i], 0);
//         RB_REMOVE(page_tree, &Mm_KernelContext.pages, &pages[i]);
//     }
//     Mm_Allocator->Free(Mm_Allocator, pages, sizeof(page)*nPages);
// }
// obos_status swap_resv(struct swap_device* dev, uintptr_t *id, size_t nPages)
// {
//     if (!dev || !id || !nPages)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     struct metadata* metadata = dev->metadata;
//     if (!metadata)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     if (metadata->hdr.magic != OBOS_SWAP_HEADER_MAGIC)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     size_t nBytes = nPages*OBOS_PAGE_SIZE;
//     if (metadata->hdr.freelist.freeBytes < nBytes)
//         return OBOS_STATUS_NOT_ENOUGH_MEMORY;
//     size_t access_size = sizeof(obos_swap_free_region);
//     if (access_size % metadata->blkSize)
//         access_size += (metadata->blkSize-(access_size%metadata->blkSize));
//     size_t accessSizePages = (access_size/OBOS_PAGE_SIZE)+(access_size%OBOS_PAGE_SIZE != 0);
//     // uint8_t* buf = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, access_size, nullptr);
//     page* pages = nullptr;
//     uint8_t* buf = map(Mm_AllocatePhysicalPages(accessSizePages, 1, nullptr), accessSizePages, &pages);
//     size_t free_off = metadata->hdr.freelist.head;
//     const size_t base_offset = metadata->vn->flags & VFLAGS_PARTITION ? metadata->vn->partitions[0].off : 0;
//     uintptr_t offset = (free_off+base_offset) / metadata->blkSize;
//     metadata->driver->header.ftable.read_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//     obos_swap_free_region* curr = (void*)buf;
//     off_t addend = 0;
//     while (curr->size < nBytes && free_off)
//     {
//         offset = (free_off+base_offset) / metadata->blkSize;
//         metadata->driver->header.ftable.read_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//         free_off = curr->next;
//         if ((curr->size + sizeof(*curr)) >= nBytes)
//         {
//             nBytes -= sizeof(*curr);
//             addend -= sizeof(*curr);
//             break;
//         }
//     }
//     if (!free_off)
//     {
//         unmap(accessSizePages, pages);
//         return OBOS_STATUS_NOT_ENOUGH_MEMORY;
//     }
//     curr->size -= nBytes;
//     curr->size -= curr->size%metadata->blkSize;
//     if (!curr->size)
//     {
//         // Unlink the node.
//         page* other_pages = nullptr;
//         uint8_t* other_buf = map(Mm_AllocatePhysicalPages(accessSizePages, 1, nullptr), accessSizePages, &other_pages);
//         uint64_t next = curr->next;
//         uint64_t prev = curr->prev;
//         curr->next = 0;
//         curr->prev = 0;
//         curr->size = 0;
//         curr->sizeof_this = 0;
//         if (next)
//         {
//             metadata->driver->header.ftable.read_sync(metadata->desc, other_buf, access_size/metadata->blkSize, (next+base_offset) / metadata->blkSize, nullptr);
//             obos_swap_free_region* next_reg = (void*)other_buf;
//             next_reg->prev = prev;
//             metadata->driver->header.ftable.write_sync(metadata->desc, other_buf, access_size/metadata->blkSize, (next+base_offset) / metadata->blkSize, nullptr);
//         }
//         if (prev)
//         {
//             metadata->driver->header.ftable.read_sync(metadata->desc, other_buf, access_size/metadata->blkSize, (prev+base_offset) / metadata->blkSize, nullptr);
//             obos_swap_free_region* next_reg = (void*)other_buf;
//             next_reg->next = next;
//             metadata->driver->header.ftable.write_sync(metadata->desc, other_buf, access_size/metadata->blkSize, (prev+base_offset) / metadata->blkSize, nullptr);
//         }
//         bool flush_hdr = true;
//         if (metadata->hdr.freelist.head == offset)
//         {
//             metadata->hdr.freelist.head = next;
//             flush_hdr = true;
//         }
//         if (metadata->hdr.freelist.tail == offset)
//         {
//             metadata->hdr.freelist.tail = prev;
//             flush_hdr = true;
//         }
//         if (flush_hdr)
//         {
//             memcpy(other_buf, &metadata->hdr, metadata->blkSize);
//             metadata->driver->header.ftable.write_sync(metadata->desc, other_buf, access_size/metadata->blkSize, 0, nullptr);
//         }
//         unmap(accessSizePages, other_pages);
//     }
//     *id = (free_off+curr->size+addend);
//     metadata->driver->header.ftable.write_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//     unmap(accessSizePages, pages);
//     return OBOS_STATUS_SUCCESS;
// }
// obos_status swap_free(struct swap_device* dev, uintptr_t  id, size_t nPages, size_t swapOff)
// {
//     if (!dev || !id || !nPages)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     if (!dev || !id || !nPages)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     struct metadata* metadata = dev->metadata;
//     if (!metadata)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     if (metadata->hdr.magic != OBOS_SWAP_HEADER_MAGIC)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     size_t nBytes = nPages*OBOS_PAGE_SIZE;
//     size_t access_size = sizeof(obos_swap_free_region);
//     id += swapOff;
//     if (access_size % metadata->blkSize)
//         access_size += (metadata->blkSize-(access_size%metadata->blkSize));
//     size_t accessSizePages = (access_size/OBOS_PAGE_SIZE)+(access_size%OBOS_PAGE_SIZE != 0);
//     page* pages = nullptr;
//     const size_t base_offset = metadata->vn->flags & VFLAGS_PARTITION ? metadata->vn->partitions[0].off : 0;
//     uint8_t* buf = map(Mm_AllocatePhysicalPages(accessSizePages, 1, nullptr), accessSizePages, &pages);
//     obos_swap_free_region free = {};
//     free.sizeof_this = sizeof(obos_swap_free_region);
//     if (free.sizeof_this % metadata->blkSize)
//         free.sizeof_this += (metadata->blkSize-(free.sizeof_this%metadata->blkSize));
//     free.size = nBytes-(metadata->blkSize != 1 ? 0 : sizeof(obos_swap_free_region));
//     if (!metadata->hdr.freelist.head)
//         metadata->hdr.freelist.head = id;
//     if (metadata->hdr.freelist.tail)
//     {
//         uintptr_t offset = (metadata->hdr.freelist.tail+base_offset) / metadata->blkSize;
//         metadata->driver->header.ftable.read_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//         obos_swap_free_region* curr = (void*)buf;
//         curr->next = id;
//         metadata->driver->header.ftable.write_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//     }
//     free.prev = metadata->hdr.freelist.tail;
//     memzero(buf, access_size);
//     metadata->hdr.freelist.tail = id;
//     memcpy(&free, buf, sizeof(free));
//     uintptr_t offset = (id+base_offset) / metadata->blkSize;
//     metadata->driver->header.ftable.write_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//     memzero(buf, sizeof(free));
//     memcpy(&metadata->hdr, buf, sizeof(free));
//     metadata->driver->header.ftable.write_sync(metadata->desc, buf, access_size/metadata->blkSize, offset, nullptr);
//     unmap(accessSizePages, pages);
//     return OBOS_STATUS_SUCCESS;
// }
// obos_status swap_write(struct swap_device* dev, uintptr_t  id, uintptr_t phys, size_t nPages, size_t offsetBytes)
// {
//     if (!dev || !id || !nPages)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     struct metadata* metadata = dev->metadata;
//     if (!metadata)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     if (metadata->hdr.magic != OBOS_SWAP_HEADER_MAGIC)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     const size_t base_offset = metadata->vn->flags & VFLAGS_PARTITION ? metadata->vn->partitions[0].off : 0;
//     size_t offset = (base_offset+id+offsetBytes)/metadata->blkSize;
//     page* pages = nullptr;
//     void* virt = map(phys, nPages, &pages);
//     obos_status status = metadata->driver->header.ftable.write_sync(metadata->desc, virt, (nPages*OBOS_PAGE_SIZE)/metadata->blkSize, offset, nullptr);
//     unmap(nPages, pages);
//     return status;
// }
// obos_status swap_read(struct swap_device* dev, uintptr_t  id, uintptr_t phys, size_t nPages, size_t offsetBytes)
// {
//     if (!dev || !id || !nPages)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     struct metadata* metadata = dev->metadata;
//     if (!metadata)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     if (metadata->hdr.magic != OBOS_SWAP_HEADER_MAGIC)
//         return OBOS_STATUS_INVALID_ARGUMENT;
//     const size_t base_offset = metadata->vn->flags & VFLAGS_PARTITION ? metadata->vn->partitions[0].off : 0;
//     size_t offset = (base_offset+id+offsetBytes);
//     if (offset % metadata->blkSize)
//         offset += (metadata->blkSize-(offset%metadata->blkSize));
//     offset /= metadata->blkSize;
//     page* pages = nullptr;
//     void* virt = map(phys, nPages, &pages);
//     obos_status status = metadata->driver->header.ftable.read_sync(metadata->desc, virt, (nPages*OBOS_PAGE_SIZE)/metadata->blkSize, offset, nullptr);
//     unmap(nPages, pages);
//     return status;
// }
OBOS_WEAK obos_status deinit_dev(struct swap_device* dev);
obos_status MmH_InitializeDiskSwapDevice(swap_dev *dev, void* vnode)
{
    fd file = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Vfs_FdOpenVnode(&file, vnode, FD_OFLAGS_UNCACHED|FD_OFLAGS_READ|FD_OFLAGS_WRITE)))
        return status;
    const size_t blkSize = Vfs_FdGetBlkSz(&file);
    if (blkSize > OBOS_PAGE_SIZE)
        return OBOS_STATUS_UNIMPLEMENTED;
    const size_t filesize = file.vn->filesize;
    struct obos_swap_header hdr = {};
    uint8_t* buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, blkSize, nullptr);
    if (obos_is_error(status = Vfs_FdRead(&file, buf, blkSize, nullptr)))
    {
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize);
        return status;
    }
    memcpy(&hdr, buf, sizeof(hdr));
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize);
    if (hdr.magic != OBOS_SWAP_HEADER_MAGIC)
    {
        Vfs_FdClose(&file);
        return OBOS_STATUS_INVALID_FILE;
    }
    if (hdr.header_size < sizeof(hdr) || (blkSize == 1 ? hdr.header_size > sizeof(hdr) : hdr.header_size > blkSize))
    {
        Vfs_FdClose(&file);
        return OBOS_STATUS_INVALID_FILE;
    }
    if (hdr.size > (filesize-hdr.header_size))
    {
        Vfs_FdClose(&file);
        return OBOS_STATUS_INVALID_FILE;
    }
    if (hdr.header_version > OBOS_SWAP_HEADER_VERSION)
    {
        Vfs_FdClose(&file);
        return OBOS_STATUS_MISMATCH;
    }
    if (hdr.flags & OBOS_SWAP_HEADER_DIRTY)
    {
        // Reset the freelist.
        OBOS_Log("%s: Swap header dirty! This could be because of a power failure or a forced shutdown. Resetting free list...\n", __func__);
        obos_swap_free_region free = {};
        free.size = hdr.size - hdr.header_size;
        free.next = free.prev = 0;
        free.sizeof_this = sizeof(obos_swap_free_region);
        if (free.sizeof_this % blkSize)
            free.sizeof_this += (blkSize-(free.sizeof_this%blkSize));
        hdr.freelist.head = hdr.header_size;
        hdr.freelist.tail = hdr.header_size;
        hdr.freelist.nNodes = 1;
        buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, blkSize, nullptr);
        hdr.flags &= ~OBOS_SWAP_HEADER_DIRTY;
        memcpy(buf, &hdr, sizeof(hdr));
        Vfs_FdSeek(&file, 0, SEEK_SET);
        Vfs_FdWrite(&file, buf, blkSize, nullptr);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize);
    }
    else 
    {
        hdr.flags |= OBOS_SWAP_HEADER_DIRTY;
        buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, blkSize, nullptr);
        memcpy(buf, &hdr, sizeof(hdr));
        Vfs_FdSeek(&file, 0, SEEK_SET);
        Vfs_FdWrite(&file, buf, blkSize, nullptr);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize);
    }
    Vfs_FdClose(&file);
    dev->metadata = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(struct metadata), nullptr);
    struct metadata* metadata = (struct metadata*)dev->metadata;
    metadata->vn = vnode;
    metadata->hdr = hdr;
    metadata->blkSize = blkSize;
    mount* const point = file.vn->mount_point ? file.vn->mount_point : file.vn->un.mounted;
    driver_id* driver = file.vn->vtype == VNODE_TYPE_REG ? point->fs_driver->driver : nullptr;
    if (file.vn->vtype == VNODE_TYPE_CHR || file.vn->vtype == VNODE_TYPE_BLK)
        driver = file.vn->un.device->driver;
    metadata->driver = driver;
    dev->metadata = metadata;
    // dev->swap_resv = swap_resv;
    // dev->swap_free = swap_free;
    // dev->swap_write = swap_write;
    // dev->swap_read = swap_read;
    return OBOS_STATUS_SUCCESS;
}
obos_status MmH_InitializeDiskSwap(void* vn_)
{
    vnode* vn = vn_;
    size_t filesize = vn->filesize;
    if (filesize == sizeof(obos_swap_header))
        return OBOS_STATUS_INVALID_ARGUMENT;
    fd file = {};
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = Vfs_FdOpenVnode(&file, vn, FD_OFLAGS_UNCACHED|FD_OFLAGS_READ|FD_OFLAGS_WRITE)))
        return status;
    const size_t blkSize = Vfs_FdGetBlkSz(&file);
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
    free.size = hdr.size - hdr.header_size;
    free.next = free.prev = 0;
    free.sizeof_this = sizeof(obos_swap_free_region);
    if (free.sizeof_this % blkSize)
        free.sizeof_this += (blkSize-(free.sizeof_this%blkSize));
    hdr.freelist.head = hdr.header_size;
    hdr.freelist.tail = hdr.header_size;
    hdr.freelist.nNodes++;
    hdr.freelist.freeBytes = free.size;
    Vfs_FdSeek(&file, 0, SEEK_SET);
    uint8_t* buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, blkSize, nullptr);
    memcpy(buf, &hdr, sizeof(hdr));
    Vfs_FdWrite(&file, buf, blkSize, nullptr);
    memzero(buf, sizeof(hdr));
    memcpy(buf, &free, sizeof(free));
    Vfs_FdWrite(&file, buf, blkSize, nullptr);
    Vfs_FdClose(&file);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf, blkSize*2);
    return OBOS_STATUS_SUCCESS;
}
