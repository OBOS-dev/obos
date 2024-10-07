/*
 * oboskrnl/allocators/basic_allocator.c
 *
 * Copyright (c) 2024 Omar Berrow
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

#include <sanitizers/asan.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

#if __x86_64__
#	include <arch/x86_64/pmm.h>
#elif defined(__m68k__)
#	include <arch/m68k/pmm.h>
#endif

static uintptr_t round_up(uintptr_t x, size_t to)
{
	if (x % to)
		return x + (to - (x % to));
	return x;
}

static void set_status(obos_status* p, obos_status to)
{
	if (p)
		*p = to;
}
struct safe_spinlock
{
	spinlock* lock;
	uint8_t oldIrql;
};
static void Lock(struct safe_spinlock* l)
{
	l->oldIrql = Core_SpinlockAcquireExplicit(l->lock, IRQL_DISPATCH, false);
}
static void Unlock(struct safe_spinlock* l)
{
	Core_SpinlockRelease(l->lock, l->oldIrql);
}
#define makeSafeLock(name, This) struct safe_spinlock name; name.lock = &((This)->lock); Lock(&name);
static OBOS_NO_KASAN void* allocateBlock(basic_allocator* This, size_t size, int* blockSource, obos_status* status)
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
			uintptr_t phys = Mm_AllocatePhysicalPages(nPages, 1, status);
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
		void* ret = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, size, 0, ((allocator_info*)This == OBOS_NonPagedPoolAllocator ? VMA_FLAGS_NON_PAGED : 0), nullptr, status);
		if (!ret)
			return nullptr;
		if ((allocator_info*)This == OBOS_NonPagedPoolAllocator)
			memzero(ret, size); // To avoid unneccessary page faults.
		*blockSource = BLOCK_SOURCE_VMA;
		return ret;
	}
	void* blk = OBOS_BasicMMAllocatePages(size, status);
	if (!blk)
		return nullptr;
	*blockSource = BLOCK_SOURCE_BASICMM;
	blk = (basicalloc_region*)(((uintptr_t)blk + 0xf) & ~0xf);
	memzero(blk, size);
	return blk;
}
static OBOS_NO_KASAN basicalloc_region* allocateNewRegion(basic_allocator* This, size_t size, obos_status* status)
{
	size = round_up(size, OBOS_PAGE_SIZE * 4);
	size += sizeof(basicalloc_region) + sizeof(basicalloc_node);
	size_t initialSize = size;
	int blockSource = 0;
	basicalloc_region* blk = (basicalloc_region*)allocateBlock(This, size, &blockSource, status);
	if (!blk)
		return nullptr;
	memzero(blk, sizeof(*blk));
	if (This == (void*)Mm_Allocator)
		OBOS_ASSERT(blockSource!=BLOCK_SOURCE_VMA);
#if OBOS_KASAN_ENABLED
	blk->This = This;
#endif
	blk->magic = PAGEBLOCK_MAGIC;
	blk->size = initialSize+sizeof(basicalloc_region);
	basicalloc_node* n = (basicalloc_node*)(blk + 1);
	memzero(n, sizeof(*n));
	n->magic = MEMBLOCK_MAGIC;
	n->size = initialSize;
	n->next = n->prev = nullptr;
	n->_containingRegion = blk;
	blk->biggestFreeNode = n;
	blk->free.tail = blk->free.head = n;
	blk->free.nNodes++;
	blk->nFreeBytes += n->size;
	blk->blockSource = blockSource;
	if (This->regionTail)
		This->regionTail->next = blk;
	if (!This->regionHead)
		This->regionHead = blk;
	blk->prev = This->regionTail;
	This->regionTail = blk;
	// printf("linked %p\nblk->prev %p\nblk->next %p\nThis %p\n", blk, blk->prev, blk->next, This);
	This->nRegions++;
	This->totalMemoryAllocated += blk->size;
	return blk;
}
static OBOS_NO_KASAN void freeRegion(basic_allocator* This, basicalloc_region* block)
{
	if (block->prev)
		block->prev->next = block->next;
	if (block->next)
		block->next->prev = block->prev;
	if (This->regionHead == block)
		This->regionHead = block->next;
	if (This->regionTail == block)
		This->regionTail = block->prev;
	This->nRegions--;
	if (block->prev)
		OBOS_ASSERT(block->prev->next != block);
	if (block->next)
		OBOS_ASSERT(block->next->prev != block);
	OBOS_ASSERT(This->regionHead != block);
	OBOS_ASSERT(This->regionTail != block);
	OBOS_ASSERT(block->blockSource != BLOCK_SOURCE_INVALID);
	This->totalMemoryAllocated -= block->size;
	// printf("alloc: freeing blk %p\nThis: %p\nblk->This %p\n", block, This, block->This);
	switch (block->blockSource) 
	{
		case BLOCK_SOURCE_BASICMM:
		{
			OBOS_BasicMMFreePages(block, block->size);
			break;
		}
		case BLOCK_SOURCE_VMA:
		{
			Mm_VirtualMemoryFree(&Mm_KernelContext, block, block->size);
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
			size_t nPages = block->size / OBOS_PAGE_SIZE;
			if (block->size % OBOS_PAGE_SIZE)
				nPages++;
			Mm_FreePhysicalPages(phys, nPages);
			break;
		}
		default:
			OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "(possible?) Region corruption in allocator (struct basic_allocator*) %p, region %p. Invalid block source: %d.\n", This, block, block->blockSource);
	}
}
static OBOS_NO_KASAN void* Allocate(allocator_info* This_, size_t size, obos_status* status)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC || !size)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}
	basic_allocator* This = (basic_allocator*)This_;
	makeSafeLock(lock, This);
	set_status(status, OBOS_STATUS_SUCCESS);
	size = round_up(size, 0x10);
#if OBOS_KASAN_ENABLED
	size += 0x100 /* add 256 byte shadow space */;
#endif
	// First, find a region.
	basicalloc_region* from = nullptr;
	basicalloc_region* start = This->regionHead;
tryAgain:
	for (basicalloc_region* r = start; r; )
	{
		if (!r->free.nNodes || !r->biggestFreeNode)
			goto end;
		if (r->biggestFreeNode->size >= size)
		{
			from = r;
			break;
		}

	end:
		r = r->next;
	}
	if (!from)
		from = allocateNewRegion(This, size, status);
	if (!from)
	{
		Unlock(&lock);
		return nullptr; // uh-oh.
	}
	// Then, use that region's first free node with a size big enough to handle this allocation.
	basicalloc_node* freeNode = nullptr;
	for (basicalloc_node* n = from->free.head; n; )
	{
		if (n->magic != MEMBLOCK_MAGIC || n->size > from->size || n->_containingRegion != from)
			OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "Memory corruption detected for block %p. Dumping basicalloc_node contents.\nn->magic: 0x%08x, n->size: %ld, n->_containingRegion: 0x%p, n->next: 0x%p, n->prev: 0x%p, allocAddr: 0x%p\n", n, n->magic, n->size, n->_containingRegion, n->next, n->prev, OBOS_NODE_ADDR(n));
		if (n->size == size)
		{
			freeNode = n;
			if (freeNode->prev)
				freeNode->prev->next = freeNode->next;
			if (freeNode->next)
				freeNode->next->prev = freeNode->prev;
			if (from->free.head == n)
				from->free.head = n->next;
			if (from->free.tail == n)
				from->free.tail = n->prev;
			from->free.nNodes--;
			if (from->biggestFreeNode == freeNode)
			{
				basicalloc_node* res = nullptr;
				for (basicalloc_node* n = from->free.head; n; )
				{
					if (!res || n->size > res->size)
						res = n;

					n = n->next;
				}
				from->biggestFreeNode = res;
			}
			break;
		}
		if (n->size >= (size + sizeof(basicalloc_node)))
		{
			// This'll work as long as we suballocate within the basicalloc_node.
			n->size -= size + sizeof(basicalloc_node);
			if (!n->size)
			{
				if (n->prev)
					n->prev->next = n->next;
				if (n->next)
					n->next->prev = n->prev;
				if (from->free.head == n)
					from->free.head = n->next;
				if (from->free.tail == n)
					from->free.tail = n->prev;
				from->free.nNodes--;
				if (from->biggestFreeNode == freeNode)
				{
					basicalloc_node* res = nullptr;
					for (basicalloc_node* n = from->free.head; n; )
					{
						if (!res || n->size > res->size)
							res = n;

						n = n->next;
					}
					from->biggestFreeNode = res;
				}
			}
			basicalloc_node* newNode = (basicalloc_node*)((char*)OBOS_NODE_ADDR(n) + n->size);
			freeNode = newNode;
			break;
		}

		n = n->next;
	}
	if (!freeNode)
	{
		start = from->next;
		from = nullptr;
		freeNode = nullptr;
		goto tryAgain;
	}
	memzero(freeNode, sizeof(*freeNode));
	freeNode->next = freeNode->prev = nullptr;
	freeNode->magic = MEMBLOCK_MAGIC;
	freeNode->_containingRegion = from;
	freeNode->size = size;
	if (from->allocated.tail)
		from->allocated.tail->next = freeNode;
	if (!from->allocated.head)
		from->allocated.head = freeNode;
	freeNode->prev = from->allocated.tail;
	from->allocated.tail = freeNode;
	from->allocated.nNodes++;
	from->nFreeBytes -= size;
	Unlock(&lock);
#if OBOS_KASAN_ENABLED
	memset((uint8_t*)OBOS_NODE_ADDR(freeNode) + size - 256, OBOS_ASANPoisonValues[ASAN_POISON_ALLOCATED], 256);
	if (memcmp_b(OBOS_NODE_ADDR(freeNode), OBOS_ASANPoisonValues[ASAN_POISON_FREED], 8))
		memzero(OBOS_NODE_ADDR(freeNode), freeNode->size);
#endif
	return OBOS_NODE_ADDR(freeNode);
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
static OBOS_NO_KASAN OBOS_NO_UBSAN void* Reallocate(allocator_info* This_, void* base, size_t newSize, obos_status* status)
{
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
	{
		set_status(status, OBOS_STATUS_INVALID_ARGUMENT);
		return nullptr;
	}
	if (!newSize)
	{
		size_t objSize = 0;
		This_->QueryBlockSize(This_, base, &objSize);
		This_->Free(This_, base, objSize);
		return nullptr;
	}
	if (!base)
		return This_->Allocate(This_, newSize, status);
	newSize = round_up(newSize, 0x10);
	// NOTE: Memory corruption can happen with KASAN enabled if QueryObjectSize() doesn't remove the shadow space size from the size returned.
	// Because of this, make sure to subtract the shadow space size if you ever decide to change this code to use the node directly.
	size_t objSize = 0;
	This_->QueryBlockSize(This_, base, &objSize);
	if (objSize == SIZE_MAX)
		return nullptr;
	if (objSize == newSize)
		return base;
	if (newSize < objSize)
	{
		basicalloc_node* n = ((basicalloc_node*)base - 1);
		memzero((char*)base + objSize, objSize-newSize);
		n->size = newSize;
		return base;
	}
	// The block is bigger.
	void* newBlock = This_->Allocate(This_, newSize, status);
	memcpy(newBlock, base, objSize);
	set_status(status, This_->Free(This_, base, objSize));
	return newBlock;
}
static OBOS_NO_KASAN OBOS_NO_UBSAN obos_status Free(allocator_info* This_, void* base, size_t nBytes)
{
	OBOS_UNUSED(nBytes);
	if (!This_ || This_->magic != OBOS_BASIC_ALLOCATOR_MAGIC)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!base)
		return OBOS_STATUS_SUCCESS;
	basicalloc_node* n = ((basicalloc_node*)base - 1);
	if (n->magic != MEMBLOCK_MAGIC)
		return OBOS_STATUS_MISMATCH;
	basicalloc_region* r = (basicalloc_region*)n->_containingRegion;
	if (!r)
		return OBOS_STATUS_INTERNAL_ERROR;
#if OBOS_KASAN_ENABLED
	if ((allocator_info*)r->This != This_)
		OBOS_ASANReport((uintptr_t)__builtin_return_address(0), (uintptr_t)base, nBytes, ASAN_AllocatorMismatch, false);
#endif
	makeSafeLock(lock, (basic_allocator*)This_);
	if (n->prev)
		n->prev->next = n->next;
	if (n->next)
		n->next->prev = n->prev;
	if (r->allocated.head == n)
		r->allocated.head = n->next;
	if (r->allocated.tail == n)
		r->allocated.tail = n->prev;
	if (!(--r->allocated.nNodes))
	{
		freeRegion((basic_allocator*)This_, r);
		Unlock(&lock);
		return OBOS_STATUS_SUCCESS;
	}
	n->next = n->prev = nullptr;
	if (r->free.tail)
		r->free.tail->next = n;
	if (!r->free.head)
		r->free.head = n;
	n->prev = r->free.tail;
	r->free.tail = n;
	r->free.nNodes++;
	r->nFreeBytes += n->size;
	if (!r->biggestFreeNode && r->free.nNodes)
	{
		Unlock(&lock);
		return OBOS_STATUS_INTERNAL_ERROR;
	}
	if (!r->biggestFreeNode || n->size > r->biggestFreeNode->size)
		r->biggestFreeNode = n;
#if OBOS_KASAN_ENABLED
	void* volatile b = base;
	memset(b, OBOS_ASANPoisonValues[ASAN_POISON_FREED], n->size);
#endif
	Unlock(&lock);
	return OBOS_STATUS_SUCCESS;
}
static OBOS_NO_KASAN OBOS_NO_UBSAN obos_status QueryBlockSize(allocator_info* This, void* base, size_t* nBytes)
{
	if (!This || This->magic != OBOS_BASIC_ALLOCATOR_MAGIC || !nBytes || !base)
		return OBOS_STATUS_INVALID_ARGUMENT;
	const basicalloc_node* n = ((const basicalloc_node*)base - 1);
	if (n->magic != MEMBLOCK_MAGIC)
		return OBOS_STATUS_MISMATCH;
	if (!n->_containingRegion)
		return OBOS_STATUS_MISMATCH;
#if OBOS_KASAN_ENABLED
	*nBytes = n->size - 256;
#else
	*nBytes = n->size;
#endif
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
