/*
 * oboskrnl/mm//initial_swap.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <mm/swap.h>
#include <mm/page.h>
#include <mm/pmm.h>
#include <mm/initial_swap.h>
#include <mm/alloc.h>

#include <utils/tree.h>

#include <irq/irql.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

#define SWAP_HEADER_MAGIC 0x535741504844524D

typedef struct swap_free_handle {
    struct swap_free_handle *next, *prev;
    uintptr_t hnd;
} swap_free_handle;
typedef struct swap_page
{
    uintptr_t key;
    void* buffer;
    size_t sz;
    RB_ENTRY(swap_page) node;
} swap_page;
typedef RB_HEAD(swap_page_tree, swap_page) swap_page_tree;
static int swap_page_compare(const swap_page* a_, const swap_page* b_)
{
    swap_page* a = (swap_page*)a_;
    swap_page* b = (swap_page*)b_;
    return (a->key < b->key) ? -1 : ((a->key > b->key) ? 1 : 0);
}
RB_GENERATE_STATIC(swap_page_tree, swap_page, node, swap_page_compare);

typedef struct swap_header
{
    uint64_t magic;
    spinlock lock;
    long size;
    long bytesLeft;
    struct {
        swap_free_handle *head, *tail;
        size_t nNodes;
        uintptr_t bump;
    } free_handles;
    swap_page_tree pages;
    swap_page_tree pages_huge;
} swap_header;
typedef struct swap_mem_tag
{
    allocator_info* allocator;
    size_t sz;
} swap_mem_tag;
static void* swap_malloc(size_t sz)
{
    allocator_info* alloc = Mm_Allocator ? Mm_Allocator : OBOS_KernelAllocator;
    swap_mem_tag* tag = alloc->ZeroAllocate(alloc, 1, sz+sizeof(swap_mem_tag), nullptr);
    tag->allocator = alloc;
    tag->sz = sz+sizeof(swap_mem_tag);
    return tag + 1;
}
static void swap_libc_free(void* buf)
{
    swap_mem_tag* tag = (swap_mem_tag*)buf;
    tag--;
    tag->allocator->Free(tag->allocator, tag, tag->sz);
}
#define PAGE_SHIFT_HUGE (__builtin_ctz(OBOS_HUGE_PAGE_SIZE))
#define PAGE_SHIFT (__builtin_ctz(OBOS_PAGE_SIZE))
static obos_status swap_resv(struct swap_device* dev, uintptr_t *id, bool huge_page)
{
    swap_header* hdr = dev->metadata;
    const long sz = huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    if (hdr->bytesLeft < sz)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        OBOS_Error("%s: Not enough space in swap.\n", __func__);
        return OBOS_STATUS_NOT_ENOUGH_MEMORY;
    }
    uintptr_t found = 0;
    if (hdr->free_handles.head)
    {
        swap_free_handle* curr = hdr->free_handles.head;
        found = curr->hnd;
        hdr->free_handles.head = curr->next; // curr == head
        if (hdr->free_handles.tail == curr)
            hdr->free_handles.tail = curr->prev;
        if (curr->prev)
            curr->prev->next = curr->next;
        if (curr->next)
            curr->next->prev = curr->prev;
        hdr->free_handles.nNodes--;
    }
    else
        found = hdr->free_handles.bump++;

    swap_page *pg = swap_malloc(sizeof(swap_page));
    pg->key = found << (huge_page ? PAGE_SHIFT_HUGE : PAGE_SHIFT);
    pg->sz = sz;
    pg->buffer = swap_malloc(sz);
    RB_INSERT(swap_page_tree, huge_page ? &hdr->pages_huge : &hdr->pages, pg);

    hdr->bytesLeft -= sz;
    if (hdr->bytesLeft < 0)
        OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "In-Ram SWAP corruption. hdr->bytesLeft < 0. bytesLeft: %ld\nThis is a bug, report it, or fix it yourself and send a PR.\n", hdr->bytesLeft);
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    *id = found << (huge_page ? PAGE_SHIFT_HUGE : PAGE_SHIFT);
    return OBOS_STATUS_SUCCESS;
}
static void swap_free_impl(void* item)
{
    swap_page* pg = (swap_page*)item;
    swap_libc_free(pg->buffer);
    pg->sz = 0;
    pg->key = 0;
    swap_libc_free(item);
}
static obos_status swap_free(struct swap_device* dev, uintptr_t id, bool huge_page)
{
    OBOS_UNUSED(huge_page);
    swap_header* hdr = dev->metadata;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    swap_page what = {.key=id};
    swap_page* item = RB_FIND(swap_page_tree, huge_page ? &hdr->pages_huge : &hdr->pages, &what);
    if (item)
    {
        RB_REMOVE(swap_page_tree, huge_page ? &hdr->pages_huge : &hdr->pages, item);
        swap_free_impl(item);
        hdr->bytesLeft += (huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
        swap_free_handle* hnd = swap_malloc(sizeof(swap_free_handle));
        memzero(hnd, sizeof(*hnd));
        hnd->hnd = id >> (huge_page ? PAGE_SHIFT_HUGE : PAGE_SHIFT);
        if (!hdr->free_handles.head)
            hdr->free_handles.head = hnd;
        if (hdr->free_handles.tail)
            hdr->free_handles.tail->next = hnd;
        hnd->prev = hdr->free_handles.tail;
        hdr->free_handles.tail = hnd;
        hdr->free_handles.nNodes++;
    }
    if (hdr->bytesLeft > hdr->size)
        OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "In-Ram SWAP corruption. hdr->bytesLeft > hdr->size. bytesLeft: %ld, size: %ld\nThis is a bug, report it, or fix it yourself and send a PR.\n", hdr->bytesLeft, hdr->size);
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_write(struct swap_device* dev, uintptr_t id, page* in)
{
    swap_header* hdr = dev->metadata;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    swap_page what = {.key=id};
    const swap_page* pg = RB_FIND(swap_page_tree, in->flags & PHYS_PAGE_HUGE_PAGE ? &hdr->pages_huge : &hdr->pages, &what);
    if (!pg)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    void* buffer = pg->buffer;
    memcpy(buffer, MmS_MapVirtFromPhys(in->phys), pg->sz);
    pg = nullptr;
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_read(struct swap_device* dev, uintptr_t id, page* into)
{
    swap_header* hdr = dev->metadata;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    swap_page what = {.key=id};
    const swap_page* pg = RB_FIND(swap_page_tree, into->flags & PHYS_PAGE_HUGE_PAGE ? &hdr->pages_huge : &hdr->pages, &what);
    if (!pg)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    void* buffer = pg->buffer;
    pg = nullptr;
    memcpy(MmS_MapVirtFromPhys(into->phys), buffer, pg->sz);
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status Mm_InitializeInitialSwapDevice(swap_dev* dev, size_t size)
{
    if (size < (sizeof(swap_header) + OBOS_HUGE_PAGE_SIZE))
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_Log("Initializing initial swap device with a size of %ld\n", size);
    dev->metadata = swap_malloc(sizeof(swap_header));
    swap_header* hdr = dev->metadata;
    memzero(hdr, sizeof(*hdr));
    hdr->size = size-sizeof(swap_header);
    hdr->bytesLeft = hdr->size;
    hdr->lock = Core_SpinlockCreate();
    dev->swap_resv = swap_resv;
    dev->swap_free = swap_free;
    dev->swap_write = swap_write;
    dev->swap_read = swap_read;
    return OBOS_STATUS_SUCCESS;
}
