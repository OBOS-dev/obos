/*
 * oboskrnl/mm/disk_swap.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
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
#include <vfs/irp.h>

#include <irq/irql.h>

#include <mm/alloc.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/pmm.h>
#include <mm/disk_swap.h>

#include <utils/tree.h>

struct metadata {
    vnode* vn;
    uint64_t freelist_head;
    uint32_t magic; // same as DISK_SWAP_MAGIC
};

#define PAGE_SHIFT(huge) __builtin_ctz((huge) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE)
#define BLOCKS_PER_PAGE(block_size, huge) (((huge) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE) / (block_size))

static uint64_t block_id_to_lba(struct metadata* data, uintptr_t id, bool huge_page)
{
    return (id >> PAGE_SHIFT(huge_page)) * BLOCKS_PER_PAGE(data->vn->blkSize, huge_page);
}
static uintptr_t lba_to_block_id(struct metadata* data, uint64_t lba, bool huge_page)
{
    return (lba / BLOCKS_PER_PAGE(data->vn->blkSize, huge_page)) << PAGE_SHIFT(huge_page);
}

static const driver_header* get_driver(vnode* vn)
{
    mount* point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO || vn->vtype == VNODE_TYPE_SOCK)
        driver = &vn->un.device->driver->header;
    return driver;
}

static obos_status read_freelist_node(struct metadata* data, uint64_t curr_lba, disk_swap_node* out)
{
    char buff[data->vn->blkSize];
    memzero(buff, sizeof(buff));
    const driver_header* hdr = get_driver(data->vn);
    obos_status status = hdr->ftable.read_sync(data->vn->desc, buff, 1, curr_lba, nullptr);
    if (obos_is_success(status))
        memcpy(out, buff, sizeof(*out));
    return status;
}
static obos_status write_freelist_node(struct metadata* data, uint64_t curr_lba, const disk_swap_node* in)
{
    char buff[data->vn->blkSize];
    memzero(buff, sizeof(buff));
    memcpy(buff, in, sizeof(*in));
    const driver_header* hdr = get_driver(data->vn);
    return hdr->ftable.write_sync(data->vn->desc, buff, 1, curr_lba, nullptr);
}

static obos_status swap_resv(struct swap_device* dev, uintptr_t* id, bool huge_page)
{
    if (!dev || !id)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    struct metadata* data = dev->metadata;
    if (!data || data->magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
        
    size_t nPages = huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    size_t alignment = nPages;

    if (!data->freelist_head)
        return OBOS_STATUS_NO_SPACE;

    uint64_t lba = 0;

    disk_swap_node curr_data = {};
    uint64_t curr_lba = data->freelist_head;
    disk_swap_node prev_data = {};
    uint64_t prev_lba = 0;
    size_t alignment_diff = 0;
    do 
    {
        read_freelist_node(data, curr_lba, &curr_data);
        
        alignment_diff = (((curr_lba + (alignment-1)) & ~(alignment-1)) - curr_lba) / BLOCKS_PER_PAGE(data->vn->blkSize, false);
        if (curr_data.nPages >= (nPages+alignment_diff))
        {
            lba = curr_lba;
            break;
        }

        prev_data = curr_data;
        prev_lba = curr_lba;
        curr_lba = curr_data.next_lba;
    } while(curr_lba);

    curr_data.nPages -= nPages+alignment_diff;
    write_freelist_node(data, curr_lba, &curr_data);
    if (!curr_data.nPages)
    {
        if (curr_lba == data->freelist_head)
            data->freelist_head = curr_data.next_lba;
        if (prev_lba)
        {
            prev_data.next_lba = curr_data.next_lba;
            write_freelist_node(data, prev_lba, &prev_data);
        }
    }
    lba = curr_lba+curr_data.nPages*BLOCKS_PER_PAGE(data->vn->blkSize, false);

    *id = lba_to_block_id(data, lba, huge_page);
    return OBOS_STATUS_SUCCESS;
}

static obos_status swap_free(struct swap_device* dev, uintptr_t id, bool huge_page)
{
    if (!dev || !id)
        return OBOS_STATUS_INVALID_ARGUMENT;

    struct metadata* data = dev->metadata;
    if (!data || data->magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    disk_swap_node node = {};
    node.nPages = huge_page ? OBOS_HUGE_PAGE_SIZE/OBOS_PAGE_SIZE : 1;
    node.next_lba = data->freelist_head;

    data->freelist_head = block_id_to_lba(data, id, huge_page);
    return write_freelist_node(data, data->freelist_head, &node);
}

static obos_status swap_write(struct swap_device* dev, uintptr_t id, page* pg)
{
    if (!dev || !id || !pg)
        return OBOS_STATUS_INVALID_ARGUMENT;

    struct metadata* data = dev->metadata;
    if (!data || data->magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    uint32_t blkOffset = block_id_to_lba(data, id, pg->flags & PHYS_PAGE_HUGE_PAGE);
    uint32_t blkCount = BLOCKS_PER_PAGE(data->vn->blkSize, pg->flags & PHYS_PAGE_HUGE_PAGE);

    const driver_header* hdr = get_driver(data->vn);
    return hdr->ftable.write_sync(data->vn->desc, MmS_MapVirtFromPhys(pg->phys), blkCount, blkOffset, nullptr);
}

static obos_status swap_read(struct swap_device* dev, uintptr_t id, page* pg)
{
    if (!dev || !id || !pg)
        return OBOS_STATUS_INVALID_ARGUMENT;

    struct metadata* data = dev->metadata;
    if (!data || data->magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    uint32_t blkOffset = block_id_to_lba(data, id, pg->flags & PHYS_PAGE_HUGE_PAGE);
    uint32_t blkCount = BLOCKS_PER_PAGE(data->vn->blkSize, pg->flags & PHYS_PAGE_HUGE_PAGE);

    const driver_header* hdr = get_driver(data->vn);
    return hdr->ftable.read_sync(data->vn->desc, MmS_MapVirtFromPhys(pg->phys), blkCount, blkOffset, nullptr);
}

static obos_status deinit_dev(struct swap_device* dev)
{
    if (!dev)
        return OBOS_STATUS_INVALID_ARGUMENT;

    struct metadata* data = dev->metadata;
    if (!data || data->magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;

    return Free(OBOS_NonPagedPoolAllocator, dev->metadata, sizeof(struct metadata));
}

obos_status Mm_MakeDiskSwap(partition* part)
{
    if (!part)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    vnode* vn = part->vn;
    
    size_t blockCount = vn->filesize/vn->blkSize;
    struct disk_swap_header hdr = {};
    hdr.reserved_block_count = OBOS_PAGE_SIZE/vn->blkSize;
    if (hdr.reserved_block_count >= blockCount)
        return OBOS_STATUS_NO_SPACE; // No space for anything but metadata
    hdr.magic = DISK_SWAP_MAGIC;
    hdr.version = DISK_SWAP_VERSION;

    OBOS_ENSURE(vn);

    fd file = {};
    obos_status st = Vfs_FdOpenVnode(&file, vn, FD_OFLAGS_WRITE);
    if (obos_is_error(st))
        return st;
    st = Vfs_FdWrite(&file, &hdr, sizeof(hdr), nullptr);
    Vfs_FdClose(&file);

    return st;
}

obos_status Mm_InitializeDiskSwap(swap_dev* dev, partition* part)
{
    if (!dev || !part)
        return OBOS_STATUS_INVALID_ARGUMENT;

    vnode* vn = part->vn;
    struct disk_swap_header hdr = {};
    fd file = {};
    obos_status st = Vfs_FdOpenVnode(&file, vn, FD_OFLAGS_READ);
    if (obos_is_error(st))
        return st;
    st = Vfs_FdRead(&file, &hdr, sizeof(hdr), nullptr);
    Vfs_FdClose(&file);
    if (obos_is_error(st))
        return st;
    
    if (hdr.magic != DISK_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_FILE;
    
    if (hdr.flags & DISK_SWAP_FLAGS_HIBERNATE)
        return OBOS_STATUS_INVALID_FILE;
    
    struct metadata* data = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(struct metadata), nullptr);
    data->freelist_head = hdr.reserved_block_count;
    data->vn = vn;
    data->magic = DISK_SWAP_MAGIC;

    size_t blockCount = vn->filesize/vn->blkSize;
    disk_swap_node node = {};
    node.nPages = (blockCount-hdr.reserved_block_count) / BLOCKS_PER_PAGE(vn->blkSize, false);
    write_freelist_node(data, data->freelist_head, &node);

    dev->metadata = data;
    dev->swap_resv = swap_resv;
    dev->swap_free = swap_free;
    dev->swap_write = swap_write;
    dev->swap_read = swap_read;
    dev->deinit_dev = deinit_dev;

    return OBOS_STATUS_SUCCESS;
}