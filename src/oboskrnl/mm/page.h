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
    struct context* owner;       // The context that this page is in.
    RB_ENTRY(page) rb_node;      // The rb-tree node.
    struct page_node ln_node;    // The linked-list node.
    struct                       //
    {                            //
        bool present : 1;        // If set, the page is present.
        bool rw : 1;             // If set, the page can be written
        bool user : 1;           // If set, the page can be accessed in user mode.
        bool touched : 1;        // If set, the page has been read or written.
        bool huge_page : 1;      // If set, the page is a huge page.
        bool executable : 1;     // If set, the page can be executed.
    } prot;                      // The protection of the page.
    bool pageable : 1;           // If set, the page is pageable.
    bool pagedOut : 1;           // If set, the page is paged out.
    bool inWorkingSet : 1;       // If set, the page is in the working-set of its context.
    bool isGuardPage : 1;        // If set, the page is a guard page.
    bool allocated : 1;          // If set, this object was allocated by Mm_Allocator.
    uint8_t age : 8;             // The page's age
    uintptr_t addr : PTR_BITS;   // The page's address.
    uintptr_t swapId : PTR_BITS; // The page's swap allocation id. Only valid if pagedOut == true.
} page;
typedef struct page_list
{
    page_node *head, *tail;
    size_t nNodes;
} page_list;
typedef RB_HEAD(page_tree, page) page_tree;
inline static int pg_cmp_pages(const page* left, const page* right)
{
    if (left->addr == right->addr)
        return 0;
    return (left->addr < right->addr) ? -1 : 1;
    // return (intptr_t)left->addr - (intptr_t)right->addr;
}
RB_PROTOTYPE(page_tree, page, rb_node, pg_cmp_pages);
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
} while(0)
