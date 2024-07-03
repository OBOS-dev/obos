/*
 * oboskrnl/mm/page.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/tree.h>

typedef struct page
{
    struct context* owner;     // The context that this page is in.
    RB_ENTRY(page) rb_node;    // The rb-tree node.
    struct                     //
    {                          //
        bool present : 1;      // If set, the page is present.
        bool rw : 1;           // If set, the page can be written
        bool user : 1;         // If set, the page can be accessed in user mode.
        bool touched : 1;      // If set, the page has been read or written.
        bool huge_page : 1;    // If set, the page is a huge page.
        bool executable : 1;   // If set, the page can be executed.
    } prot;                    // The protection of the page.
    bool pageable : 1;         // If set, the page is pageable.
    bool pagedOut : 1;         // If set, the page is paged out.
    uint8_t age : 8;           // The page's age
    uintptr_t addr : PTR_BITS; // The page's address.
} page;
typedef struct page_node
{
    struct page_node *next, *prev;
    page* data;
} page_node;
typedef struct page_list
{
    page_node *head, *tail;
    size_t nNodes;
} page_list;
typedef RB_HEAD(page_tree, page) page_tree;
inline int rb_cmp_pages(page* left, page* right)
{
    return (intptr_t)left->addr - (intptr_t)right->addr;
}
RB_PROTOTYPE(page_tree, page, rb_node, rb_cmp_pages);