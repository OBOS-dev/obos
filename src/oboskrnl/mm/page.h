/*
 * oboskrnl/mm/page.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/tree.h>

typedef struct page_node
{
    struct page_node *next, *prev;
    struct page* data;
} page_node;
typedef struct page
{
    struct context* owner;                  // The context that this page is in.
    RB_ENTRY(page) rb_node;                 // The rb-tree node.
    page_node ln_node;                      // The linked-list node.
    struct pagecache_mapped_region* region; // A region of a vnode that is supposed to be mapped here.
    bool isPrivateMapping;                  // Only valid if region != nullptr. Set to true if the file mapping is a private mapping.
    struct                                  //
    {                                       //
        bool present : 1;                   // If set, the page is present.
        bool rw : 1;                        // If set, the page can be written
        bool user : 1;                      // If set, the page can be accessed in user mode.
        bool touched : 1;                   // If set, the page has been read or written.
        bool huge_page : 1;                 // If set, the page is a huge page.
        bool executable : 1;                // If set, the page can be executed.
        bool uc : 1;                        // If set, this page is uncacheable.
        bool ro : 1;                        // If set, this page was originally allocated as read-only. This is only used in CoW pages as of now.
    } prot;                                 // The protection of the page.
    size_t workingSets : 16;                // The amount of working sets the page is in.
    bool pageable : 1;                      // If set, the page is pageable.
    bool pagedOut : 1;                      // If set, the page is paged out.
    bool isGuardPage : 1;                   // If set, the page is a guard page.
    bool allocated : 1;                     // If set, this object was allocated by Mm_Allocator.
    bool reserved : 1;                      // If set, this object is reserved memory (i.e., not backed by anything).
    uint8_t age : 8;                        // The page's age
    uintptr_t addr : PTR_BITS;              // The page's address.
    uintptr_t swapId : PTR_BITS;            // The page's swap allocation id. Only valid if pagedOut == true.
    struct page* next_copied_page;          // If CoW is enabled on this page, this contains the pointer to the next page we're sharing data with.
    struct page* prev_copied_page;          // If CoW is enabled on this page, this contains the pointer to the previous page we're sharing data with.
} page;
typedef struct page_list
{
    page_node *head, *tail;
    size_t nNodes;
} page_list;
typedef RB_HEAD(page_tree, page) page_tree;
#pragma GCC push_options
#pragma GCC optimize ("-O0")
inline static int pg_cmp_pages(const page* left, const page* right)
{
    if (left->addr == right->addr)
        return 0;
    return (left->addr < right->addr) ? -1 : 1;
    // return (intptr_t)left->addr - (intptr_t)right->addr;
}
#pragma GCC pop_options
RB_PROTOTYPE_INTERNAL(page_tree, page, rb_node, pg_cmp_pages, __attribute__((noinline)));
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
