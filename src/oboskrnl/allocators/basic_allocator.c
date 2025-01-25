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
	if (Mm_IsInitialized())
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
		void* ret = Mm_QuickVMAllocate(size, (void*)This == (void*)OBOS_NonPagedPoolAllocator);
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
	sz += sizeof(region);
	enum blockSource src = 0;
	region* reg = init_mmap(sz, alloc, &src);
	if (!reg)
		return 0;
#if OBOS_KASAN_ENABLED
	memzero(reg, sz);
#endif
	reg->block_source = src;
	reg->start = (void*)((uintptr_t)reg + init_pgsize());
	reg->sz = sz-sizeof(region);
	reg->magic = REGION_MAGIC;
	reg->alloc = alloc;
	reg->alloc_size = sz_node;
	append_node(c->region_list, reg);
	if (sz_node == reg->sz)
	{
		reg->nBlocks = 1;
		append_node(c->free, (freelist_node*)reg->start);
	}
	else
	{
		reg->nBlocks = (reg->sz/sz_node);
		for (size_t i = 0; i < reg->nBlocks; i++)
		{
			freelist_node* node = (freelist_node*)(((uintptr_t)reg->start) + i*sz_node);
			append_node(c->free, node);
		}
	}

	reg->nFree = reg->nBlocks;

	return 1;
}

static OBOS_NO_UBSAN void* Allocate(allocator_info* This_, size_t nBytes, obos_status* status)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC || !nBytes)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}

	basic_allocator* This = (basic_allocator*)This_;

	if (nBytes <= 16)
		nBytes = 16;
	else
		nBytes = (size_t)1 << (64-__builtin_clzll(nBytes-1));
	if (nBytes > (4*1024*1024))
		return NULL; // invalid argument

	size_t cache_index = __builtin_ctzll(nBytes)-4;
	volatile cache* c = &This->caches[cache_index];

	irql oldIrql = lock((cache*)c);

	void* ret = c->free.tail;
	if (!ret)
	{
		if (!allocate_region(This, (cache*)c, cache_index))
		{
			unlock((cache*)c, oldIrql);
			return NULL; // OOM
		}

		ret = c->free.tail;
	}

	if ((c->free.tail)->next)
		(c->free.tail)->next = nullptr;
	remove_node(c->free, c->free.tail);

	unlock((cache*)c, oldIrql);
	// printf("%s: %ld -> %p\n", __func__, nBytes, ret);
	return ret;
}
static OBOS_NO_KASAN void* ZeroAllocate(allocator_info* This, size_t nObjects, size_t bytesPerObject, obos_status* status)
{
	if (!This || This->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}
	size_t size = bytesPerObject * nObjects;
	return memzero(Allocate(This, size, status), size);
}

static OBOS_NO_KASAN OBOS_NO_UBSAN void* Reallocate(allocator_info* This_, void* blk, size_t new_size, size_t old_size, obos_status* status)
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

static OBOS_NO_KASAN OBOS_NO_UBSAN obos_status Free(allocator_info* This_, void* blk, size_t nBytes)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
		return OBOS_STATUS_INVALID_ARGUMENT;

	if (!blk || !nBytes)
		return OBOS_STATUS_SUCCESS;

	OBOS_ENSURE(nBytes != 0xaa);

	if (nBytes <= 16)
		nBytes = 16;
	else
		nBytes = (size_t)1 << (64-__builtin_clzll(nBytes-1));

	// printf("%s: %p %ld\n", __func__, blk, nBytes);

	if (nBytes > (4*1024*1024))
		return OBOS_STATUS_INVALID_ARGUMENT; // invalid argument

	basic_allocator* alloc = (basic_allocator*)This_;

	size_t cache_index = __builtin_ctzll(nBytes)-4;
	cache* c = &alloc->caches[cache_index];

	memzero(blk, sizeof(freelist_node));

	if (nBytes >= init_pgsize())
	{
		region* reg = (void*)((uintptr_t)blk - init_pgsize());
		OBOS_ENSURE(reg->magic == REGION_MAGIC);
		OBOS_ENSURE((allocator_info*)reg->alloc == This_);
		OBOS_ENSURE(reg->alloc_size == nBytes);

		irql oldIrql = lock(c);
		remove_node(c->region_list, reg);
		unlock(c, oldIrql);

		init_munmap(reg->block_source, reg, reg->sz);
	}
	else
	{
		const uintptr_t blki = (uintptr_t)blk;
		region* volatile reg = (void*)((blki - (blki % init_pgsize())) - init_pgsize());
		OBOS_ENSURE(reg->magic == REGION_MAGIC);
		OBOS_ENSURE((allocator_info*)reg->alloc == This_);
		if (reg->alloc_size != nBytes)
		{
			nBytes = reg->alloc_size;
			cache_index = __builtin_ctzll(nBytes)-4;
			c = &alloc->caches[cache_index];
		}

		irql oldIrql = lock(c);

		memset(blk, 0xaa, nBytes);
		if ((reg->nFree + 1) == reg->nBlocks)
		{
			for (size_t i = 0; i < reg->nBlocks; i++)
			{
				freelist_node* node = (freelist_node*)(((uintptr_t)reg->start) + i*(1<<cache_index));
				remove_node(c->free, node);
			}
			remove_node(c->region_list, reg);

			init_munmap(reg->block_source, reg, reg->sz);
		} else
			append_node(c->free, (freelist_node*)blk);

		unlock(c, oldIrql);
	}
	return OBOS_STATUS_SUCCESS;
}

static OBOS_NO_KASAN OBOS_NO_UBSAN obos_status QueryBlockSize(allocator_info* This, void* base, size_t* nBytes)
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
