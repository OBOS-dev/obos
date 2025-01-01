/*
 * oboskrnl/allocators/basic_allocator.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <allocators/base.h>

#include <locks/spinlock.h>

#define MEMBLOCK_MAGIC  0x6AB450AA
#define PAGEBLOCK_MAGIC	0x768AADFC
#define MEMBLOCK_DEAD   0x3D793CCD
#define OBOS_BASIC_ALLOCATOR_MAGIC (0x7E046A92E7735)

#define OBOS_NODE_ADDR(n) ((void*)(n + 1))

enum blockSource
{
	BLOCK_SOURCE_INVALID = -1, // It is an error to get this.
	BLOCK_SOURCE_PHYSICAL_MEMORY, // See allocateBlock "if ((allocator_info*)This == Mm_Allocator)"
	BLOCK_SOURCE_BASICMM, // OBOS_BasicMMAllocatePages
	BLOCK_SOURCE_VMA, // Mm_AllocateVirtualMemory
};

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
	size_t nFree;
	size_t nBlocks;
	uint64_t magic;
	enum blockSource block_source;
	struct basic_allocator* alloc;
	struct region *next, *prev;
} region;

typedef struct cache {
	freelist free;
	struct {
		region *head, *tail;
		size_t nNodes;
	} region_list;
	spinlock lock;
} cache;

typedef struct basic_allocator
{
	allocator_info header;
	cache caches[28];
} basic_allocator;

obos_status OBOSH_ConstructBasicAllocator(basic_allocator* This);
