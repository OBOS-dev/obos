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

#include <utils/tree.h>
#include <utils/hashmap.h>

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
} swap_page;
static int swap_page_compare(const void* a_, const void* b_, void* udata)
{
    OBOS_UNUSED(udata);
    swap_page* a = (swap_page*)a_;
    swap_page* b = (swap_page*)b_;
    return (a->key < b->key) ? -1 : ((a->key > b->key) ? 1 : 0);
}
static uint64_t swap_page_hash(const void *item, uint64_t seed0, uint64_t seed1) 
{
    const swap_page* a = item;
    return hashmap_sip(&a->key, sizeof(a->key), seed0, seed1);
}
typedef struct swap_header
{
    uint64_t magic;
    spinlock lock;
    long size;
    long bytesLeft;
    struct hashmap* hashmap;
    struct {
        swap_free_handle *head, *tail;
        size_t nNodes;
        uintptr_t bump;
    } free_handles;
} swap_header;
typedef struct swap_mem_tag
{
    allocator_info* allocator;
} swap_mem_tag;
static void* swap_malloc(size_t sz)
{
    allocator_info* alloc = OBOS_NonPagedPoolAllocator ? OBOS_NonPagedPoolAllocator : OBOS_KernelAllocator;
    swap_mem_tag* tag = alloc->ZeroAllocate(alloc, 1, sz+sizeof(swap_mem_tag), nullptr);
    tag->allocator = alloc;
    return tag + 1;
}
static void* swap_realloc(void* buf, size_t sz)
{
    swap_mem_tag* tag = (swap_mem_tag*)buf;
    tag--;
    return tag->allocator->Reallocate(tag->allocator, tag, sz+sizeof(swap_mem_tag), nullptr);
}
static void swap_libc_free(void* buf)
{
    swap_mem_tag* tag = (swap_mem_tag*)buf;
    tag--;
    size_t size = 0;
    tag->allocator->QueryBlockSize(tag->allocator, buf, &size);
    tag->allocator->Free(tag->allocator, buf, size);
}
#define PAGE_SHIFT (__builtin_clz(OBOS_HUGE_PAGE_SIZE > 0 ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE))
static obos_status swap_resv(struct swap_device* dev, uintptr_t *id, bool huge_page)
{
    swap_header* hdr = dev->metadata;
    const long sz = huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    if (hdr->bytesLeft < sz)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
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
    swap_page pg = {};
    pg.key = found;
    pg.sz = sz;
    pg.buffer = swap_malloc(sz);
    hashmap_set(hdr->hashmap, &pg);
    hdr->bytesLeft -= sz;
    if (hdr->bytesLeft < 0)
        OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "In-Ram SWAP corruption. hdr->bytesLeft < 0. bytesLeft: %ld\nThis is a bug, report it, or fix it yourself and send a PR.\n", hdr->bytesLeft);
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    *id = found << PAGE_SHIFT;
    if ((*id >> PAGE_SHIFT) != found)
        OBOS_Panic(OBOS_PANIC_ASSERTION_FAILED, "File %s:%d: Whoops ((*id >> PAGE_SHIFT) != found)\n", __FILE__, __LINE__);
    return OBOS_STATUS_SUCCESS;
}
static void swap_free_impl(void* item)
{
    swap_page* pg = (swap_page*)item;
    swap_libc_free(pg->buffer);
    pg->sz = 0;
    pg->key = 0;
}
static obos_status swap_free(struct swap_device* dev, uintptr_t id)
{
    swap_header* hdr = dev->metadata;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    swap_page what = {.key=id};
    const void* item = hashmap_delete(hdr->hashmap, &what);
    if (item)
    {
        hdr->bytesLeft += what.sz;
        swap_free_handle* hnd = swap_malloc(sizeof(swap_free_handle));
        memzero(hnd, sizeof(*hnd));
        hnd->hnd = id;
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
    const swap_page* pg = hashmap_get(hdr->hashmap, &what);
    if (!pg)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    void* buffer = pg->buffer;
    pg = nullptr;
    memcpy(buffer, MmS_MapVirtFromPhys(in->phys), pg->sz);
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_read(struct swap_device* dev, uintptr_t id, page* into)
{
    swap_header* hdr = dev->metadata;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    swap_page what = {.key=id};
    const swap_page* pg = hashmap_get(hdr->hashmap, &what);
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
    dev->metadata = swap_malloc(sizeof(swap_header));
    swap_header* hdr = dev->metadata;
    memzero(hdr, sizeof(*hdr));
    hdr->size = size-sizeof(swap_header);
    hdr->bytesLeft = hdr->size;
    hdr->lock = Core_SpinlockCreate();
    hdr->hashmap = hashmap_new_with_allocator(swap_malloc, swap_realloc, swap_libc_free, sizeof(struct swap_allocation), 256, 0, 0, swap_page_hash, swap_page_compare, swap_free_impl, nullptr);
    dev->swap_resv = swap_resv;
    dev->swap_free = swap_free;
    dev->swap_write = swap_write;
    dev->swap_read = swap_read;
    return OBOS_STATUS_SUCCESS;
}