/*
 * oboskrnl/mm/page.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/tree.h>
#include <utils/list.h>

typedef struct page_protection
{
    bool present : 1;      // If set, the page is present.
    bool rw : 1;           // If set, the page can be written
    bool user : 1;         // If set, the page can be accessed in user mode.
    bool huge_page : 1;    // If set, the page is a huge page.
    bool executable : 1;   // If set, the page can be executed.
    bool uc : 1;           // If set, this page is uncacheable.
    bool ro : 1;           // If set, this page was originally allocated as read-only. This is only for decoration, and is only really guaranteed to be set in page ranges.
    bool is_swap_phys : 1; // If set, the physical address of the page is actually a swap id. On x86-64, this uses bit 9 of the PTE.
} page_protection;

typedef struct page_node
{
    struct page_node *next, *prev;
    struct page_info *data;
} page_node;

typedef struct page_info
{
    struct page_node ln_node; // for the 'struct page' list.
    uintptr_t virt;
    uintptr_t phys;
    struct page_range* range; // note: reserved for user mode versions of this struct
    page_protection prot;
    bool dirty : 1;
    bool accessed : 1;
} page_info;

typedef enum phys_page_flags
{
    PHYS_PAGE_STANDBY = BIT(0),
    PHYS_PAGE_DIRTY = BIT(1),
    PHYS_PAGE_HUGE_PAGE = BIT(2),
} phys_page_flags;

typedef RB_HEAD(phys_page_tree, page) phys_page_tree;
RB_PROTOTYPE(phys_page_tree, page, rb_node, phys_page_cmp);
typedef LIST_HEAD(phys_page_list, struct page) phys_page_list;
LIST_PROTOTYPE(phys_page_list, struct page, lnk_node);

typedef struct page
{
    RB_ENTRY(page) rb_node;
    // only valid if the page is dirty/standby.
    LIST_NODE(phys_page_list, struct page) lnk_node;
    uintptr_t phys;

    _Atomic(size_t) refcount;
    // A reference count of pages that have this page paged in.
    // Must always be <= refcount.
    _Atomic(size_t) pagedCount;

    struct {
        page_node *head, *tail;
        size_t nNodes;
    } virt_pages /* virtual pages with a reference to us */;

    uintptr_t swap_id;
    phys_page_flags flags;

    enum {
        COW_DISABLED,
        COW_SYMMETRIC, // for fork, etc.
        COW_ASYMMETRIC, // for CoW on a private page.
    } cow_type;
} page;

typedef LIST_HEAD(swap_allocation_list, struct swap_allocation) swap_allocation_list;
LIST_PROTOTYPE(swap_allocation_list, struct swap_allocation, node);
extern swap_allocation_list Mm_SwapAllocations;

// represents a swap allocation, as well as the physical page that it uses, if it was already read.
typedef struct swap_allocation
{
    LIST_NODE(swap_allocation_list, struct swap_allocation) node;
    uintptr_t id; // the key
    size_t refs;
    page* phys; // if !phys, this page must be read from swap.
    struct swap_device* provider;
} swap_allocation;
OBOS_NODISCARD swap_allocation* MmH_LookupSwapAllocation(uintptr_t id);
swap_allocation* MmH_AddSwapAllocation(uintptr_t id);
void MmH_RefSwapAllocation(swap_allocation* alloc);
void MmH_DerefSwapAllocation(swap_allocation* alloc);

inline static int phys_page_cmp(struct page* lhs, struct page* rhs)
{
    return (lhs->phys < rhs->phys) ? -1 : (lhs->phys == rhs->phys ? 0 : 1);
}

// Adds a reference to the page.
page* MmH_PgAllocatePhysical(bool phys32, bool huge);
page* MmH_AllocatePage(uintptr_t phys, bool huge);
void MmH_RefPage(page* buf);
void MmH_DerefPage(page* buf);
extern phys_page_tree Mm_PhysicalPages;
extern size_t Mm_PhysicalMemoryUsage; // Current physical memory usage in bytes.

typedef struct page_range
{
    uintptr_t virt;
    size_t size;
    page_protection prot;
    struct pagecache_mapped_region* mapped_here;
    RB_ENTRY(page_range) rb_node;
    struct {
        struct working_set_node *head, *tail;
        size_t nNodes;
    } working_set_nodes;
    size_t size_committed;
    struct context* ctx;
    bool pageable : 1;
    bool hasGuardPage : 1;
    bool reserved : 1;
    bool can_fork : 1; // see madvise(MADV_DONTFORK)
    bool phys32 : 1; // See VMA_FLAGS_32BITPHYS
    bool kernelStack : 1; // See Mm_AllocateKernelStack
    union {
        struct context* userContext; // valid if kernelStack != nullptr
    } un;
} page_range;

typedef struct working_set_node
{
    struct working_set_node *next, *prev;
    struct working_set_entry* data;
} working_set_node;

typedef struct working_set_entry
{
    struct {
        uintptr_t virt;
        page_protection prot;
        page_range *range;
    } info;
    struct working_set_node pr_node; // page range node
    uint16_t workingSets;
#if OBOS_PAGE_REPLACEMENT_AGING
    uint8_t age;
#endif
    // Set to true when this needs to be freed.
    bool free : 1;
    _Atomic(size_t) refs;
} working_set_entry;
typedef struct page_list
{
    page_node *head, *tail;
    size_t nNodes;
} page_list;
typedef RB_HEAD(page_tree, page_range) page_tree;

#pragma GCC push_options
#pragma GCC optimize ("-O0")
#define in_range(ra,rb,x) (((x) >= (ra)) && ((x) < (rb)))
inline static int pg_cmp_pages(const page_range* left, const page_range* right)
{
    if (in_range(right->virt, right->virt+right->size, left->virt))
        return 0;
    if (left->virt < right->virt)
        return -1;
    if (left->virt > right->virt)
        return 1;
    return 0;
}
#undef in_range
#pragma GCC pop_options

RB_PROTOTYPE_INTERNAL(page_tree, page_range, rb_node, pg_cmp_pages, __attribute__((noinline)));
#define APPEND_PAGE_NODE(list, node) do {\
	(node)->next = nullptr;\
	(node)->prev = nullptr;\
	if ((list).tail)\
		(list).tail->next = (node);\
	if (!(list).head)\
		(list).head = (node);\
	(node)->prev = ((list).tail);\
	(list).tail = (node);\
	(list).nNodes++;\
} while(0)
#define REMOVE_PAGE_NODE(list, node) do {\
	if ((list).tail == (node))\
		(list).tail = (node)->prev;\
	if ((list).head == (node))\
		(list).head = (node)->next;\
	if ((node)->prev)\
		(node)->prev->next = (node)->next;\
	if ((node)->next)\
		(node)->next->prev = (node)->prev;\
	(list).nNodes--;\
    (node)->next = nullptr;\
    (node)->prev = nullptr;\
} while(0)
