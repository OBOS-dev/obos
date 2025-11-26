/*
 * oboskrnl/mm/bare_map.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <cmdline.h>
#include <error.h>
#include <klog.h>

#include <mm/bare_map.h>
#include <mm/pmm.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

// Quote of the VMM:
// When I wrote this, only God and I understood what I was doing.
// Now, only God knows.

#define REGION_MAGIC_INT 0x4F424F534253434D
#define REGION_MAGIC "OBOSBSCM"

static struct
{
	basicmm_region* head;
	basicmm_region* tail;
	size_t nNodes;
} s_regionList;
static basicmm_region bump_region;
static size_t bump_offset;
static spinlock s_regionListLock;
static bool s_regionListLockInitialized;
static OBOS_PAGEABLE_FUNCTION irql Lock()
{
	if (!s_regionListLockInitialized)
	{
		s_regionListLock = Core_SpinlockCreate();
		s_regionListLockInitialized = true;
	}
	return Core_SpinlockAcquire(&s_regionListLock);
}
static OBOS_PAGEABLE_FUNCTION void Unlock(irql oldIrql)
{
	OBOS_ASSERT(s_regionListLockInitialized);
	Core_SpinlockRelease(&s_regionListLock, oldIrql);
}

void OBOSH_BasicMMSetBumpRegion(basicmm_region* region)
{
	irql oldIrql = Lock();
	bump_region = *region;
	Unlock(oldIrql);
}

OBOS_NO_KASAN OBOS_PAGEABLE_FUNCTION void* OBOS_BasicMMAllocatePages(size_t sz, obos_status* status)
{
	irql oldIrql = Lock();
	if (!bump_region.addr)
		OBOS_Panic(OBOS_PANIC_ALLOCATOR_ERROR, "BasicMM: No region specified by platform code.\n");
	sz += (OBOS_PAGE_SIZE-(sz%OBOS_PAGE_SIZE));
	if ((bump_offset + sz) >= bump_region.size)
		OBOS_Panic(OBOS_PANIC_NO_MEMORY, "BasicMM: No more space in bump allocator.\n");
	if (status)
		*status = OBOS_STATUS_SUCCESS;
	uintptr_t addr = bump_region.addr + bump_offset;
	bump_offset += sz;
	Unlock(oldIrql);
	return (void*)addr;
	// sz += sizeof(basicmm_region);
	// sz += (OBOS_PAGE_SIZE - (sz % OBOS_PAGE_SIZE));
	// irql oldIrql = Lock();
	// // Find a basicmm_region.
	// basicmm_region* node = nullptr;
	// basicmm_region* currentNode = nullptr;
	// uintptr_t lastAddress = OBOS_KERNEL_ADDRESS_SPACE_BASE;
	// uintptr_t found = 0;
	// for (currentNode = s_regionList.head; currentNode;)
	// {
	// 	uintptr_t currentNodeAddr = currentNode->addr - (currentNode->addr % OBOS_PAGE_SIZE);
	// 	if (currentNodeAddr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
	// 	{
	// 		currentNode = currentNode->next;
	// 		continue;
	// 	}
	// 	if ((currentNodeAddr - lastAddress) >= (sz + OBOS_PAGE_SIZE))
	// 	{
	// 		found = lastAddress;
	// 		break;
	// 	}
	// 	lastAddress = currentNodeAddr + currentNode->size;

	// 	currentNode = currentNode->next;
	// }
	// if (!found)
	// {
	// 	basicmm_region* currentNode = s_regionList.tail;
	// 	if (currentNode)
	// 		found = (currentNode->addr - (currentNode->addr % OBOS_PAGE_SIZE)) + currentNode->size;
	// 	else
	// 		found = OBOS_KERNEL_ADDRESS_SPACE_BASE;
	// }
	// if (!found)
	// {
	// 	if (status)
	// 		*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
	// 	return nullptr;
	// }
	// node = (basicmm_region*)found;
	// for (uintptr_t addr = found; addr < (found + sz); addr += OBOS_PAGE_SIZE)
	// {
	// 	obos_status stat = OBOS_STATUS_SUCCESS;
	// 	uintptr_t mem = Mm_AllocatePhysicalPages(1, 1, &stat);
	// 	if (stat != OBOS_STATUS_SUCCESS)
	// 	{
	// 		if (status)
	// 			*status = stat;
	// 		return nullptr;
	// 	}
	// 	OBOSS_MapPage_RW_XD((void*)addr, mem);
	// }
	// memzero(node, sizeof(*node));
	// Unlock(oldIrql);
	// OBOSH_BasicMMAddRegion(node, node + 1, sz);
	// if (status)
	// 	*status = OBOS_STATUS_SUCCESS;
	// return (node + 1);
}
OBOS_NO_KASAN OBOS_PAGEABLE_FUNCTION obos_status OBOS_BasicMMFreePages(void* base_, size_t sz)
{
	OBOS_UNUSED(base_);
	OBOS_UNUSED(sz);
	// sz += sizeof(basicmm_region);
	// sz += (OBOS_PAGE_SIZE - (sz % OBOS_PAGE_SIZE));
	// uintptr_t base = (uintptr_t)base_;
	// basicmm_region* reg = ((basicmm_region*)base - 1);
	// if (((uintptr_t)reg & ~0xfff) != (base & ~0xfff))
	// 	return OBOS_STATUS_MISMATCH;
	// if (reg->magic.integer != REGION_MAGIC_INT)
	// 	return OBOS_STATUS_MISMATCH;
	// if (reg->size != sz)
	// 	return OBOS_STATUS_INVALID_ARGUMENT;
	// irql oldIrql = Lock();
	// if (reg->next)
	// 	reg->next->prev = reg->prev;
	// if (reg->prev)
	// 	reg->prev->next = reg->next;
	// if (s_regionList.head == reg)
	// 	s_regionList.head = reg->next;
	// if (s_regionList.tail == reg)
	// 	s_regionList.tail = reg->prev;
	// s_regionList.nNodes--;
	// Unlock(oldIrql);
	// // Unmap the basicmm_region.
	// base &= ~0xfff;
	// for (uintptr_t addr = base; addr < (base + sz); addr += OBOS_PAGE_SIZE)
	// {
	// 	uintptr_t phys = 0;
	// 	OBOSS_GetPagePhysicalAddress((void*)addr, &phys);
	// 	OBOSS_UnmapPage((void*)addr);
	// 	if (phys)
	// 		Mm_FreePhysicalPages(phys, 1);
	// }
	// return OBOS_STATUS_SUCCESS;
	// Bump allocators don't do freeing.
	return OBOS_STATUS_SUCCESS;
}

OBOS_NO_KASAN OBOS_PAGEABLE_FUNCTION void OBOSH_BasicMMAddRegion(basicmm_region* node, void* base_, size_t sz)
{
	uintptr_t base = (uintptr_t)base_;
	node->magic.integer = REGION_MAGIC_INT;
	node->addr = base;
	node->size = sz;
	if (s_regionList.tail && s_regionList.tail->addr < base)
	{
		// Append it
		irql oldIrql = Lock();
		if (s_regionList.tail)
			s_regionList.tail->next = node;
		if (!s_regionList.head)
			s_regionList.head = node;
		node->prev = s_regionList.tail;
		s_regionList.tail = node;
		s_regionList.nNodes++;
		Unlock(oldIrql);
		return;
	}
	else if (s_regionList.head && s_regionList.head->addr > base)
	{
		// Prepend it
		irql oldIrql = Lock();
		if (s_regionList.head)
			s_regionList.head->prev = node;
		if (!s_regionList.tail)
			s_regionList.tail = node;
		node->next = s_regionList.head;
		s_regionList.head = node;
		s_regionList.nNodes++;
		Unlock(oldIrql);
		return;
	}
	else
	{
		irql oldIrql = Lock();
		// Find the node that this node should go after.
		basicmm_region* found = nullptr, *n = s_regionList.head;
		while (n && n->next)
		{
			if (n->addr < base &&
				n->next->addr >= node->addr)
			{
				found = n;
				break;
			}

			n = n->next;
		}
		if (!found)
		{
			// Append it
			if (s_regionList.tail)
				s_regionList.tail->next = node;
			if (!s_regionList.head)
				s_regionList.head = node;
			node->prev = s_regionList.tail;
			s_regionList.tail = node;
			s_regionList.nNodes++;
			Unlock(oldIrql);
			return;
		}
		if (found->next)
			found->next->prev = node;
		if (s_regionList.tail == found)
			s_regionList.tail = node;
		node->next = found->next;
		found->next = node;
		node->prev = found;
		s_regionList.nNodes++;
		Unlock(oldIrql);
	}
}
OBOS_PAGEABLE_FUNCTION void OBOSH_BasicMMIterateRegions(bool(*callback)(basicmm_region*, void*), void* udata)
{
	// callback(&bump_region, udata);
	// irql oldIrql = Core_SpinlockAcquireExplicit(&s_regionListLock, IRQL_DISPATCH, false);
	for (basicmm_region* cur = s_regionList.head; cur; )
	{
		if (cur->mmioRange)
			goto next;
		
		if (!callback(cur, udata))
		{
			// Core_SpinlockRelease(&s_regionListLock, oldIrql);
			return;
		}
		next:
		cur = cur->next;
	}
	// Core_SpinlockRelease(&s_regionListLock, oldIrql);
}
