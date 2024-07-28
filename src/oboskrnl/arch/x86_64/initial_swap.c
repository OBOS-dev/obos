/*
 * oboskrnl/arch/x86_64/initial_swap.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "arch/x86_64/pmm.h"
#include "irq/irql.h"
#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <mm/swap.h>
#include <mm/page.h>

#include <stdint.h>
#include <utils/tree.h>

#include <locks/spinlock.h>

#define SWAP_HEADER_MAGIC 0x535741504844524D

typedef struct swap_page
{
    size_t size;
    struct swap_page *next, *prev;
    RB_ENTRY(swap_page) rb_node;
} swap_page;
typedef RB_HEAD(swap_page_tree, swap_page) swap_page_tree;
inline static int cmp(swap_page* left, swap_page* right)
{
    if (left == right)
        return 0;
    return (intptr_t)left - (intptr_t)right;    
}
RB_GENERATE_STATIC(swap_page_tree, swap_page, rb_node, cmp);
typedef struct swap_header
{
    uint64_t magic;
    swap_page_tree pages;
    struct 
    {
        swap_page *head, *tail;
        size_t nNodes;
    } freeList;
    size_t size;
    size_t nBytesFree;
    spinlock lock;
} swap_header;
// The allocation id is simply the entry in the swap_page_tree.
static obos_status swap_resv(struct swap_device* dev, uintptr_t* id, size_t nPages)
{
    if (!dev || !id || !nPages) 
        return OBOS_STATUS_INVALID_ARGUMENT;
    swap_header* hdr = (swap_header*)dev->metadata;
    if (!hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->magic != SWAP_HEADER_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t allocSize = nPages*OBOS_PAGE_SIZE;
    size_t nBytesRequired = allocSize + sizeof(swap_page);
    if (hdr->nBytesFree < nBytesRequired)
        return OBOS_STATUS_NOT_ENOUGH_MEMORY;
    irql oldIrql = Core_SpinlockAcquireExplicit(&hdr->lock, IRQL_MASKED, true);
    swap_page* page = hdr->freeList.head;
    while(page && page->size < nBytesRequired)
        page = page->next;
    if (!page)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_ENOUGH_MEMORY; // TODO: Unfragement and try again.
    }
    page->size -= nBytesRequired;
    hdr->nBytesFree -= nBytesRequired;
    swap_page* buf = (swap_page*)((uintptr_t)page + page->size);
    if (!page->size)
    {
        if (page->next)
            page->next->prev = page->prev;
        if (page->prev)
            page->prev->next = page->next;
        if (hdr->freeList.head == page)
            hdr->freeList.head = page->next;
        if (hdr->freeList.tail == page)
            hdr->freeList.tail = page->prev;
        hdr->freeList.nNodes--;
    }
    RB_INSERT(swap_page_tree, &hdr->pages, buf);
    buf->size = allocSize;
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    *id = (uintptr_t)buf;
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_free(struct swap_device* dev, uintptr_t id, size_t nPages)
{
    OBOS_UNUSED(nPages);
    if (!dev || !nPages || !id) 
        return OBOS_STATUS_INVALID_ARGUMENT;
    swap_header* hdr = (swap_header*)dev->metadata;
    if (!hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->magic != SWAP_HEADER_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquireExplicit(&hdr->lock, IRQL_MASKED, true);
    swap_page* page = (swap_page*)id;
    if (!RB_FIND(swap_page_tree, &hdr->pages, page))
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    RB_REMOVE(swap_page_tree, &hdr->pages, page);
    if (!hdr->freeList.head)
        hdr->freeList.head = page;
    if (hdr->freeList.tail)
        hdr->freeList.tail->next = page;
    page->prev = hdr->freeList.tail;
    hdr->freeList.tail = page;
    hdr->freeList.nNodes--;
    hdr->nBytesFree += page->size;
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_write(struct swap_device* dev, uintptr_t id, uintptr_t phys, size_t nPages, size_t offsetBytes)
{
    if (!dev || !id)
        return OBOS_STATUS_INVALID_ARGUMENT;
    swap_header* hdr = (swap_header*)dev->metadata;
    if (!hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->magic != SWAP_HEADER_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquireExplicit(&hdr->lock, IRQL_MASKED, true);
    swap_page* page = (swap_page*)id;
    if (!RB_FIND(swap_page_tree, &hdr->pages, page))
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    if (!nPages)
        return OBOS_STATUS_SUCCESS;
    if (page->size < (nPages*OBOS_PAGE_SIZE))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (offsetBytes >= page->size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((offsetBytes + (nPages*OBOS_PAGE_SIZE)) > page->size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    char* buf = (char*)(page + 1);
    buf += offsetBytes;
    memcpy(buf, Arch_MapToHHDM(phys), nPages*OBOS_PAGE_SIZE);
    return OBOS_STATUS_SUCCESS;
}
static obos_status swap_read(struct swap_device* dev, uintptr_t id, uintptr_t phys, size_t nPages, size_t offsetBytes)
{
    if (!dev || !id)
        return OBOS_STATUS_INVALID_ARGUMENT;
    swap_header* hdr = (swap_header*)dev->metadata;
    if (!hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (hdr->magic != SWAP_HEADER_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquireExplicit(&hdr->lock, IRQL_MASKED, true);
    swap_page* page = (swap_page*)id;
    if (!RB_FIND(swap_page_tree, &hdr->pages, page))
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_INVALID_ARGUMENT;
    }
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    if (!nPages)
        return OBOS_STATUS_SUCCESS;
    if (page->size < (nPages*OBOS_PAGE_SIZE))
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (offsetBytes >= page->size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((offsetBytes + (nPages*OBOS_PAGE_SIZE)) > page->size)
        return OBOS_STATUS_INVALID_ARGUMENT;
    char* buf = (char*)(page + 1);
    buf += offsetBytes;
    memcpy(Arch_MapToHHDM(phys), buf, nPages*OBOS_PAGE_SIZE);
    return OBOS_STATUS_SUCCESS;
}
obos_status Arch_InitializeInitialSwapDevice(swap_dev* dev, void* buf, size_t size)
{
    if (!buf || size < (sizeof(swap_header) + OBOS_HUGE_PAGE_SIZE))
        return OBOS_STATUS_INVALID_ARGUMENT;
    dev->metadata = buf;
    dev->swap_resv = swap_resv;
    dev->swap_free = swap_free;
    dev->swap_write = swap_write;
    dev->swap_read = swap_read;
    swap_header* hdr = (swap_header*)buf;
    memzero(hdr, sizeof(*hdr));
    hdr->size = size-sizeof(swap_header);
    hdr->nBytesFree = hdr->size;
    swap_page* free = (swap_page*)(hdr+1);
    memzero(free, sizeof(*free));
    hdr->nBytesFree -= sizeof(*free);
    free->size = hdr->nBytesFree;
    hdr->freeList.head = free;
    hdr->freeList.tail = free;
    hdr->freeList.nNodes = 1;
    hdr->magic = SWAP_HEADER_MAGIC;
    return OBOS_STATUS_SUCCESS;
}