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
typedef struct basicalloc_node
{
	OBOS_ALIGNAS(0x10) uint32_t magic /* Must be MEMBLOCK_MAGIC */;
	OBOS_ALIGNAS(0x10) size_t size;
	OBOS_ALIGNAS(0x10) void* _containingRegion;
	OBOS_ALIGNAS(0x10) struct basicalloc_node* next;
	OBOS_ALIGNAS(0x10) struct basicalloc_node* prev;
} basicalloc_node;
typedef struct basicalloc_node_list
{
	basicalloc_node* head, *tail;
	size_t nNodes;
} basicalloc_node_list;
enum blockSource
{
	BLOCK_SOURCE_INVALID = -1, // It is an error to get this.
	BLOCK_SOURCE_PHYSICAL_MEMORY, // See allocateBlock "if ((allocator_info*)This == Mm_Allocator)"
	BLOCK_SOURCE_BASICMM, // OBOS_BasicMMAllocatePages
	BLOCK_SOURCE_VMA, // Mm_AllocateVirtualMemory
};
typedef struct basicalloc_region
{
	OBOS_ALIGNAS(0x10) uint32_t magic /* Must be PAGEBLOCK_MAGIC */;
	OBOS_ALIGNAS(0x10) size_t size;
	OBOS_ALIGNAS(0x10) size_t nFreeBytes;
	OBOS_ALIGNAS(0x10) basicalloc_node_list free, allocated;
	OBOS_ALIGNAS(0x10) basicalloc_node* biggestFreeNode;
	OBOS_ALIGNAS(0x10) int blockSource; // See 'enum blockSource'

	OBOS_ALIGNAS(0x10) struct basicalloc_region *next, *prev;

#ifdef OBOS_KASAN_ENABLED
	OBOS_ALIGNAS(0x10) struct basic_allocator* This;
#else
	OBOS_ALIGNAS(0x10) void* resv;
#endif
} basicalloc_region;
typedef struct basic_allocator
{
	allocator_info header;
	basicalloc_region *regionHead, *regionTail;
	size_t nRegions;
	size_t totalMemoryAllocated;
	size_t nAllocations;
	size_t nFrees;
	spinlock lock;
} basic_allocator;

obos_status OBOSH_ConstructBasicAllocator(basic_allocator* This);
