/*
 * oboskrnl/allocators/basic_allocator.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <allocators/base.h>
#include <allocators/basic_allocator.h>

#include <mm/bare_map.h>
#include <mm/alloc.h>
#include <mm/init.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <asan.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

#if __x86_64__
#	include <arch/x86_64/pmm.h>
#elif defined(__m68k__)
#	include <arch/m68k/pmm.h>
#endif

static OBOS_NO_KASAN uintptr_t round_up(uintptr_t x, size_t to)
{
	if (x % to)
		return x + (to - (x % to));
	return x;
}

static OBOS_NO_KASAN  void set_status(obos_status* p, obos_status to)
{
	if (p)
		*p = to;
}

#define makeSafeLock(name, This) struct safe_spinlock name; name.lock = &((This)->lock); Lock(&name);
static OBOS_NO_KASAN void* init_mmap(size_t size, basic_allocator* This, enum blockSource* blockSource)
{
	OBOS_ASSERT(blockSource);
	*blockSource = BLOCK_SOURCE_INVALID;
	if (obos_expect(Mm_IsInitialized(), true))
	{
		// Use the VMA, unless this is the vmm allocator.
		if ((allocator_info*)This == Mm_Allocator)
		{
			// If this calculation changes, update freeRegion
			size_t nPages = size / OBOS_PAGE_SIZE;
			if (size % OBOS_PAGE_SIZE)
				nPages++;
			uintptr_t phys = Mm_AllocatePhysicalPages(nPages, 1, nullptr);
			if (!phys)
				return nullptr;
			// Arch-specific:
			// Map the physical address to virtual addresses without the need of page nodes.
			// On x86-64, this is done using the HHDM.
			void* ret = nullptr;
#ifdef __x86_64__
			*blockSource = BLOCK_SOURCE_PHYSICAL_MEMORY;
			ret = Arch_MapToHHDM(phys);
#elif defined(__m68k__)
			*blockSource = BLOCK_SOURCE_PHYSICAL_MEMORY;
			ret = Arch_MapToHHDM(phys);
#else
#	error Unknown architecture
#endif
			memzero(ret, size);
			return ret;
		}
		
		void* ret = nullptr;
		// if (size < OBOS_HUGE_PAGE_SIZE)
		ret = Mm_QuickVMAllocate(size, (void*)This == (void*)OBOS_NonPagedPoolAllocator);
		// else
		//  	ret = Mm_VirtualMemoryAlloc(&Mm_KernelContext,
		// 								nullptr, size,
		// 								0, 
		// 								(((void*)This == (void*)OBOS_NonPagedPoolAllocator) ? VMA_FLAGS_NON_PAGED : 0) | VMA_FLAGS_HUGE_PAGE,
		// 								nullptr,
		// 								nullptr);
		if (!ret)
			return nullptr;
		memzero(ret, size);
		*blockSource = BLOCK_SOURCE_VMA;
		return ret;
	}
	void* blk = OBOS_BasicMMAllocatePages(size, nullptr);
	if (!blk)
		return nullptr;
	*blockSource = BLOCK_SOURCE_BASICMM;
	blk = (void*)(((uintptr_t)blk + 0xf) & ~0xf);
	memzero(blk, size);
	return blk;
}

size_t init_pgsize() { return OBOS_PAGE_SIZE; }

static OBOS_NO_KASAN void init_munmap(enum blockSource blockSource, void* block, size_t size)
{
	switch (blockSource)
	{
		case BLOCK_SOURCE_BASICMM:
		{
			OBOS_BasicMMFreePages(block, size);
			break;
		}
		case BLOCK_SOURCE_VMA:
		{
			Mm_VirtualMemoryFree(&Mm_KernelContext, block, size);
			break;
		}
		case BLOCK_SOURCE_PHYSICAL_MEMORY:
		{
			uintptr_t phys = 0;
			#ifdef __x86_64__
			phys = Arch_UnmapFromHHDM(block);
			#elif defined(__m68k__)
			phys = Arch_UnmapFromHHDM(block);
			#else
			#	error Unknown architecture
			#endif
			size_t nPages = size / OBOS_PAGE_SIZE;
			if (size % OBOS_PAGE_SIZE)
				nPages++;
			memset(block, 0xcc, size);
			Mm_FreePhysicalPages(phys, nPages);
			break;
		}
		default:
			OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "(possible?) Region corruption in region %p. Invalid block source: %d.\n", block, blockSource);
	}
}

#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"

#define ALLOCATOR_ALIGNMENT 0x10

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

#if 1
OBOS_NO_KASAN irql lock(cache* c)
{
	return Core_SpinlockAcquire(&c->lock);
}
OBOS_NO_KASAN void unlock(cache* c, irql oldIrql)
{
	Core_SpinlockRelease(&c->lock, oldIrql);
}
#else
irql lock(cache* c)
{
	// return Core_SpinlockAcquire(&c->lock);
	return 0;
}
void unlock(cache* c, irql oldIrql)
{
	// Core_SpinlockRelease(&c->lock, oldIrql);
}
#endif

static OBOS_NO_KASAN int allocate_region(basic_allocator* alloc, cache* c, size_t cache_index)
{
	size_t sz = 1 << (cache_index+4);
	size_t sz_node = sz;
	if (sz < init_pgsize())
		sz = init_pgsize();
	void* reg = init_mmap(sz, alloc, &alloc->blkSource);
	if (!reg)
		return 0;
#if OBOS_KASAN_ENABLED
	memzero(reg, sz);
#endif
	
	if (sz_node == sz)
		append_node(c->free, (freelist_node*)reg);
	else
	{
		size_t nBlocks = (sz/sz_node);
		for (size_t i = 0; i < nBlocks; i++)
		{
			freelist_node* node = (freelist_node*)((uintptr_t)reg + i*sz_node);
			append_node(c->free, node);
		}
	}

	return 1;
}

OBOS_NO_UBSAN void* Allocate(allocator_info* This_, size_t nBytes, obos_status* status)
{
	basic_allocator* This = (basic_allocator*)This_;

	if (nBytes <= 16)
		nBytes = 16;
	else
		nBytes = (size_t)1 << (64-__builtin_clzll(nBytes));
	if (obos_expect(nBytes > (4UL*1024*1024*1024), false))
	{
		if (status) *status = OBOS_STATUS_INVALID_ARGUMENT;
		return NULL; // invalid argument
	}

	size_t cache_index = __builtin_ctzll(nBytes)-4;
	volatile cache* c = &This->caches[cache_index];

	irql oldIrql = lock((cache*)c);

	void* ret = c->free.tail;
	if (obos_expect(!ret, false))
	{
		if (!allocate_region(This, (cache*)c, cache_index))
		{
			unlock((cache*)c, oldIrql);
			if (status) *status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
			return NULL; // OOM
		}

		ret = c->free.tail;
	}

	if ((c->free.tail)->next)
		(c->free.tail)->next = nullptr;
	remove_node(c->free, c->free.tail);

	unlock((cache*)c, oldIrql);
	return ret;
}
OBOS_NO_KASAN void* ZeroAllocate(allocator_info* This, size_t nObjects, size_t bytesPerObject, obos_status* status)
{
	if (!This || This->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}
	size_t size = bytesPerObject * nObjects;
	void* blk = Allocate(This, size, status);
	if (blk)
		return memset(blk, 0, size);
	return blk;
}

OBOS_NO_KASAN OBOS_NO_UBSAN void* Reallocate(allocator_info* This_, void* blk, size_t new_size, size_t old_size, obos_status* status)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}
	// basic_allocator* alloc = (basic_allocator*)This_;
	if (!blk)
		return Allocate(This_, new_size, status);
	if (!new_size)
	{
		This_->Free(This_, blk, old_size);
		return NULL;
	}
	void* newblk = Allocate(This_, new_size, status);
	if (!newblk)
		return blk;
	memcpy(newblk, blk, old_size);
	This_->Free(This_, blk, old_size);
	return newblk;
}

OBOS_NO_KASAN OBOS_NO_UBSAN obos_status Free(allocator_info* This_, void* blk, size_t nBytes)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
		return OBOS_STATUS_INVALID_ARGUMENT;

	if (!blk || !nBytes)
		return OBOS_STATUS_SUCCESS;

	if (nBytes <= 16)
		nBytes = 16;
	else
		nBytes = (size_t)1 << (64-__builtin_clzll(nBytes-1));

	if (nBytes > (4UL*1024*1024*1024))
		return OBOS_STATUS_INVALID_ARGUMENT; // invalid argument

	basic_allocator* alloc = (basic_allocator*)This_;

	size_t cache_index = __builtin_ctzll(nBytes)-4;
	cache* c = &alloc->caches[cache_index];

	memzero(blk, sizeof(freelist_node));

	if (obos_expect(nBytes >= init_pgsize(), false))
		init_munmap(alloc->blkSource, blk, nBytes);
	else
	{
		irql oldIrql = lock(c);
		append_node(c->free, (freelist_node*)blk);
		// oops this has been here too long
		memset(((freelist_node*)blk)+1, 0xff, nBytes-sizeof(freelist_node));
		unlock(c, oldIrql);
	}
	return OBOS_STATUS_SUCCESS;
}

OBOS_NO_KASAN OBOS_NO_UBSAN obos_status QueryBlockSize(allocator_info* This, void* base, size_t* nBytes)
{
	if (!This || This->magic != OBOS_BASIC_ALLOCATOR_MAGIC || !nBytes || !base)
		return OBOS_STATUS_INVALID_ARGUMENT;
	// We don't have the capability to do such things.
	*nBytes = 0;
	return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status OBOSH_ConstructBasicAllocator(basic_allocator* This)
{
	if (!This)
		return OBOS_STATUS_INVALID_ARGUMENT;
	This->header.magic = OBOS_BASIC_ALLOCATOR_MAGIC;
	This->header.Allocate = Allocate;
	This->header.ZeroAllocate = ZeroAllocate;
	This->header.Reallocate = Reallocate;
	This->header.Free = Free;
	This->header.QueryBlockSize = QueryBlockSize;
	return OBOS_STATUS_SUCCESS;
}
