/*
 * init/allocator.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct freelist_node {
    struct freelist_node *next, *prev;
} freelist_node;

_Static_assert(sizeof(freelist_node) <= 16, "Internal bug, report this.");

typedef struct freelist {
    freelist_node *head, *tail;
    size_t nNodes;
} freelist;

enum {
    REGION_MAGIC = 0xb49ad907c56c8
};

typedef struct region {
    void* start;
    size_t sz;
    uint64_t magic;
    struct region *next, *prev;
} region;

typedef struct cache {
    freelist free;
    struct {
        region *head, *tail;
        size_t nNodes;
    } region_list;
} cache;

#define append_node(list, node) do {\
    if (!(list).head)\
        (list).head = (node);\
    if ((list).tail)\
        (list).tail->next = (node);\
    (node)->prev = (list).tail;\
    (list).tail = (node);\
    (list).nNodes++; \
} while(0)

#define remove_node(list, node) do {\
    if ((node)->next)\
        (node)->next->prev = (node)->prev;\
    if ((node)->prev)\
        (node)->prev->next = (node)->next;\
    if ((list).head == (node))\
        (list).head = (node)->next;\
    if ((list).tail == (node))\
        (list).tail = (node)->prev;\
    (list).nNodes--;\
} while(0)

typedef struct allocator {
    cache caches[28];
} allocator;

extern void* init_mmap(size_t sz);
extern void  init_munmap(void* blk, size_t sz);
extern size_t init_pgsize();

void* init_malloc(allocator* alloc, size_t nBytes);
void* init_calloc(allocator* alloc, size_t cnt, size_t nBytes);
void* init_realloc(allocator* alloc, void* blk, size_t new_size, size_t old_size);
void  init_free(allocator* alloc, void* blk, size_t sz);

extern allocator init_allocator;

void* malloc(size_t sz);
void* calloc(size_t cnt, size_t sz);
void* realloc(void* blk, size_t sz, size_t old_size);
void  free(void* blk, size_t sz);
