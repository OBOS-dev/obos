/*
 * oboskrnl/mm/init.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include "irq/irql.h"
#include "locks/spinlock.h"
#include "mm/handler.h"
#include "scheduler/cpu_local.h"
#include <int.h>
#include <memmanip.h>
#include <error.h>
#include <klog.h>

#include <mm/context.h>
#include <mm/init.h>
#include <mm/bare_map.h>
#include <mm/pg_node.h>
#include <mm/swap.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <allocators/basic_allocator.h>
#include <allocators/base.h>

#include <stdint.h>
#include <utils/tree.h>

context* Mm_KernelContext;

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

struct register_mm_regions_udata
{   
    page_node* node_buf;
    size_t node_buf_size;
    size_t i;
};
#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
static bool register_mm_regions(basicmm_region* region, void* udata_)
{
    if (region->addr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
        return true;
    struct register_mm_regions_udata* udata = (struct register_mm_regions_udata*)udata_;
    pt_context ctx = MmS_PTContextGetCurrent();
    for (uintptr_t addr = region->addr & ~0xfff; addr < ((region->addr & ~0xfff) + region->size); )
    {
        pt_context_page_info info;
        memzero(&info, sizeof(info));
        MmS_PTContextQueryPageInfo(&ctx, addr, &info);
        bool isPageNodeBuf = 
            (addr >= round_down(udata->node_buf)) &&
            (addr < 
                round_up(((uintptr_t)udata->node_buf) + udata->node_buf_size)   
            )
        ;
        if (!MmH_AddressExcluded(addr) && !isPageNodeBuf)
        {
            OBOS_ASSERT(udata->i < (udata->node_buf_size / sizeof(page_node)));
            page_node* node = &udata->node_buf[udata->i++];
            memzero(node, sizeof(*node));
            node->lock = Core_SpinlockCreate();
            node->addr = addr;
            node->huge_page = info.huge_page;
            node->accessed = info.accessed;
            node->dirty = info.dirty;
            node->present = info.present;
            node->protection = info.protection;
            RB_INSERT(page_node_tree, &Mm_KernelContext->pageNodeTree, node);
        }
        if (info.huge_page)
            addr += OBOS_HUGE_PAGE_SIZE;
        else
            addr += OBOS_PAGE_SIZE;
    }
    return true;
}
static bool get_page_node_buf_count(basicmm_region* region, void* udata_)
{
    if (region->addr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
        return true;
    size_t* sz = (size_t*)udata_;
    if (region->size < OBOS_HUGE_PAGE_SIZE)
    {
        (*sz) += region->size/OBOS_PAGE_SIZE;
        return true;
    }
    else
    {
        pt_context ctx = MmS_PTContextGetCurrent();
        for (uintptr_t addr = region->addr & ~0xfff; addr < ((region->addr & ~0xfff) + region->size); (*sz)++)
        {
            pt_context_page_info info;
            memzero(&info, sizeof(info));
            MmS_PTContextQueryPageInfo(&ctx, addr, &info);
            if (info.huge_page)
                addr += OBOS_HUGE_PAGE_SIZE;
            else
                addr += OBOS_PAGE_SIZE;
        }
    }
    return true;
}
static obos_status init_swap_put(struct swap_dev* dev, const page_node* node, uintptr_t phys);
static obos_status init_swap_get(struct swap_dev* dev, const page_node* node, uintptr_t phys);
static OBOS_EXCLUDE_VAR_FROM_MM swap_dev init_swap_provider = { init_swap_put, init_swap_get, nullptr };
typedef struct init_swap_page_rb
{
    const page_node* node;
    uintptr_t offset;
    RB_ENTRY(init_swap_page_rb) rb_node;
}  init_swap_page_rb;
typedef RB_HEAD(swap_page_rb, init_swap_page_rb) swap_page_rb;
static OBOS_EXCLUDE_FUNC_FROM_MM int cmp_node(const init_swap_page_rb* right, const init_swap_page_rb* left)
{
    return cmp_page_node(right->node, left->node);
}
RB_PROTOTYPE_INTERNAL(swap_page_rb, init_swap_page_rb, rb_node, cmp_node, static OBOS_EXCLUDE_FUNC_FROM_MM);
typedef struct init_swap_page_lnk
{
    size_t size;
    struct init_swap_page_lnk *next, *prev;
} init_swap_page_lnk;
#define INIT_SWAP_MAGIC (0x1A4967B8CA3701)
typedef struct init_swap_header
{
    size_t size;
    size_t nBytesUsed;
    swap_page_rb swap_pages;
    uint64_t magic;
    init_swap_page_lnk *head, *tail;
    spinlock lock;
} init_swap_header;
void* MmS_InitialSwapBuffer;
size_t MmS_InitialSwapBufferSize;
static OBOS_EXCLUDE_VAR_FROM_MM bool s_initialized;
static OBOS_EXCLUDE_VAR_FROM_MM context kctx;
static basic_allocator allocator;
void Mm_InitializeAllocator()
{
    obos_status status = OBOSH_ConstructBasicAllocator(&allocator);
    OBOS_ASSERT(obos_unlikely_error(status));
    Mm_VMMAllocator = (allocator_info*)&allocator;
}
OBOS_EXCLUDE_FUNC_FROM_MM void Mm_Initialize()
{
    OBOS_ASSERT(!Mm_KernelContext);
    obos_status status = OBOS_STATUS_SUCCESS;
    Mm_KernelContext = &kctx;
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a vmm context object for the kernel. Status: %d\n", status);
    process* current = Core_GetCurrentThread()->proc;
    status = MmH_InitializeContext(Mm_KernelContext, current);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the vmm context object for the kernel. Status: %d\n", status);
    Mm_KernelContext->pt_ctx = MmS_PTContextGetCurrent();
    size_t nNodes = 0;
    OBOS_Debug("%s: Populating page node tree.\n", __func__);
    OBOS_Debug("%s: Getting page node count...\n", __func__);
    OBOSH_BasicMMIterateRegions(get_page_node_buf_count, &nNodes);
    OBOS_Debug("%s: Page node count is %lu. Populating tree...\n", __func__, nNodes);
    // nNodes += (((nNodes*sizeof(page_node)+sizeof(basicmm_region)) + 0xfff) & ~0xfff) / 0x1000;
    page_node* node_buf = OBOS_BasicMMAllocatePages(nNodes*sizeof(page_node), &status);
    if (obos_likely_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a page node buffer. Status: %d\n", status);
    struct register_mm_regions_udata udata = {
        .node_buf = node_buf,
        .node_buf_size = nNodes*sizeof(page_node),
        .i = 0,
    };
    OBOSH_BasicMMIterateRegions(register_mm_regions, &udata);
    OBOS_Debug("%s: Populated tree.\n", __func__);
    OBOS_Debug("%s: Initializing initial swap device...\n", __func__);
    init_swap_provider.data = MmS_InitialSwapBuffer;
    if (MmS_InitialSwapBufferSize < (sizeof(init_swap_header)+OBOS_HUGE_PAGE_SIZE))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Size %lu for initial swap buffer is too small. Must be at least %lu bytes in length.", MmS_InitialSwapBufferSize, sizeof(init_swap_header)+OBOS_HUGE_PAGE_SIZE);
    init_swap_header* hdr = (init_swap_header*)init_swap_provider.data;
    hdr->size = MmS_InitialSwapBufferSize;
    init_swap_page_lnk* head = (init_swap_page_lnk*)(hdr + 1);
    head->size = hdr->size - sizeof(*hdr);
    hdr->head = head;
    hdr->tail = head;
    hdr->nBytesUsed += sizeof(*hdr) + sizeof(*head);
    hdr->magic = INIT_SWAP_MAGIC;
    hdr->lock = Core_SpinlockCreate();
    Mm_SwapProvider = &init_swap_provider;
    for (size_t i = 0; i < Core_CpuCount; i++)
        Core_CpuInfo[i].currentContext = Mm_KernelContext;
    OBOS_Debug("%s: Initialized initial swap device.\n", __func__);
    s_initialized = true;
    OBOS_Debug("%s: Swapping out pages...\n", __func__);
    // Swap out the entire kernel lol.
    page_node *i = nullptr;
    // MmS_LockPageFaultHandler(true);
    irql oldIrql = Core_RaiseIrqlNoThread(IRQL_MASKED);
    RB_FOREACH(i, page_node_tree, &Mm_KernelContext->pageNodeTree)
    {
        Core_SpinlockAcquireExplicit(&i->lock, IRQL_MASKED, true);
        Mm_PageOutPage(Mm_SwapProvider, i);
     
        Core_SpinlockForcedRelease(&i->lock);
    }
    // MmS_LockPageFaultHandler(false);
    Core_LowerIrqlNoThread(oldIrql);
    OBOS_Debug("%s: Swapped out pages...\n", __func__);
}
OBOS_EXCLUDE_FUNC_FROM_MM bool Mm_Initialized()
{
    return s_initialized;
}
static OBOS_EXCLUDE_FUNC_FROM_MM obos_status init_swap_put(struct swap_dev* dev, const page_node* pnode, uintptr_t phys)
{
    if (!dev || dev->put_page != init_swap_put || !dev->data || !pnode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    init_swap_header* hdr = (init_swap_header*)dev->data;
    if (hdr->magic != INIT_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    size_t sizeRequired = sizeof(init_swap_page_rb) + (pnode->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
    if ((hdr->size - hdr->nBytesUsed) < sizeRequired)
        return OBOS_STATUS_NOT_ENOUGH_MEMORY;
    init_swap_page_lnk* node = hdr->head;
    while (node && node->size < sizeRequired)
        node = node->next;
    if (!node)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_ENOUGH_MEMORY; // TODO: Defragment and try again.
    }    
    node->size -= sizeRequired;
    if (!node->size)
    {
        if (node->prev)
            node->prev->next = node->next;
        if (node->next)
            node->next->prev = node->prev;
        if (hdr->head == node)
            hdr->head = node->next;
        if (hdr->tail == node)
            hdr->tail = node->prev;
        hdr->nBytesUsed -= sizeof(*node);
    }
    init_swap_page_rb* page = (void*)(((uintptr_t)node) + node->size);
    memzero(page, sizeof(*page));
    page->node = pnode;
    page->offset = ((uintptr_t)(page+1))-(uintptr_t)dev->data;
    void* virt = MmS_GetDM(phys);
    memcpy(page+1, (void*)virt, (pnode->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE));
    RB_INSERT(swap_page_rb, &hdr->swap_pages, page);
    hdr->nBytesUsed += sizeRequired;
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
static OBOS_EXCLUDE_FUNC_FROM_MM obos_status init_swap_get(struct swap_dev* dev, const page_node* pnode, uintptr_t phys)
{
    if (!dev || dev->get_page != init_swap_get || !dev->data || !pnode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    init_swap_header* hdr = (init_swap_header*)dev->data;
    if (hdr->magic != INIT_SWAP_MAGIC)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquire(&hdr->lock);
    init_swap_page_rb what = {.node = pnode};
    init_swap_page_rb* page = RB_FIND(swap_page_rb, &hdr->swap_pages, &what);
    if (!page)
    {
        Core_SpinlockRelease(&hdr->lock, oldIrql);
        return OBOS_STATUS_NOT_FOUND;
    }
    void* virt = MmS_GetDM(phys);
    memcpy((void*)virt, (void*)((uintptr_t)hdr + page->offset), (pnode->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE));
    RB_REMOVE(swap_page_rb, &hdr->swap_pages, page);
    // Free the node.
    OBOS_STATIC_ASSERT(sizeof(*page) >= sizeof(init_swap_page_lnk), "Invalid size.");
    init_swap_page_lnk* lnode = (init_swap_page_lnk*)page;
    memzero(lnode, sizeof(*lnode));
    if (!hdr->head)
        hdr->head = lnode;
    if (hdr->tail)
        hdr->tail->next = lnode;
    lnode->prev = hdr->tail;
    hdr->tail = lnode;
    hdr->nBytesUsed -= pnode->huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
    Core_SpinlockRelease(&hdr->lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
RB_GENERATE_INTERNAL(swap_page_rb, init_swap_page_rb, rb_node, cmp_node, static OBOS_EXCLUDE_FUNC_FROM_MM);