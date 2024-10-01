/*
	oboskrnl/mm/pmm.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

#include <sanitizers/asan.h>

#include <mm/pmm.h>
#include <mm/init.h>
#include <mm/alloc.h>
#include <mm/page.h>
#include <mm/swap.h>

struct freelist_node
{
	size_t nPages;
	struct freelist_node *next, *prev;
};
static struct freelist_node *s_head;
static struct freelist_node *s_tail;
#if OBOS_ARCHITECTURE_BITS == 64

// 32-bit region start
static struct freelist_node *s_head32;
// 32-bit region tail
static struct freelist_node *s_tail32;

#endif
static size_t s_nNodes;
size_t Mm_TotalPhysicalPages;
size_t Mm_TotalPhysicalPagesUsed;
size_t Mm_UsablePhysicalPages;
uintptr_t Mm_PhysicalMemoryBoundaries;
static spinlock lock;

obos_status Mm_InitializePMM()
{
	uintptr_t i = 0;
	if (!MmS_GetFirstPMemMapEntry(&i))
		return OBOS_STATUS_INVALID_INIT_PHASE;
	for (obos_pmem_map_entry* entry = MmS_GetFirstPMemMapEntry(&i); entry; )
	{
		uintptr_t phys = entry->pmem_map_base;
		size_t nPages = entry->pmem_map_size / OBOS_PAGE_SIZE;
		if (phys % OBOS_PAGE_SIZE)
		{
			phys = phys + (OBOS_PAGE_SIZE-(phys%OBOS_PAGE_SIZE));
			nPages--;
		}
		if (phys == 0x0)
		{
			phys = OBOS_PAGE_SIZE;
			nPages--;
		}
		Mm_TotalPhysicalPages += nPages;
		if ((phys + nPages * OBOS_PAGE_SIZE) > Mm_PhysicalMemoryBoundaries)
			Mm_PhysicalMemoryBoundaries = (phys + nPages * OBOS_PAGE_SIZE);
		if (entry->pmem_map_type != PHYSICAL_MEMORY_TYPE_USABLE || !nPages)
		{
			Mm_TotalPhysicalPagesUsed += nPages;
			entry = MmS_GetNextPMemMapEntry(entry, &i);
			continue;
		}
		Mm_UsablePhysicalPages += nPages;
		// struct freelist_node* node = (struct freelist_node*)(MmS_MapVirtFromPhys(phys));
		// memzero(node, sizeof(*node));
		// node->nPages = nPages;
		// if (s_tail)
		// 	((struct freelist_node*)(MmS_MapVirtFromPhys((uintptr_t)s_tail)))->next = (struct freelist_node*)phys;
		// if (!s_head)
		// 	s_head = (struct freelist_node*)phys;
		// node->prev = s_tail;
		// s_tail = (struct freelist_node*)phys;
		// s_nNodes++;
		OBOS_Debug("%s: Free physical memory region at 0x%p-0x%p.\n", __func__, phys, phys+nPages*OBOS_PAGE_SIZE);
		Mm_FreePhysicalPages(phys, nPages);
		entry = MmS_GetNextPMemMapEntry(entry, &i);
	}
#if OBOS_ARCHITECTURE_BITS == 64
	if (Mm_PhysicalMemoryBoundaries & 4294967295)
		Mm_PhysicalMemoryBoundaries = (Mm_PhysicalMemoryBoundaries + 4294967295) & ~4294967295;
#endif
	return OBOS_STATUS_SUCCESS;
}
static bool IsRegionSufficient(struct freelist_node* node, size_t nPages, size_t alignmentMask, size_t *nPagesRequiredP)
{
	size_t nPagesRequired = nPages;
	uintptr_t nodePhys = MmS_UnmapVirtFromPhys(node);
	nPagesRequired += (node->nPages & ((alignmentMask + 1) / OBOS_PAGE_SIZE - 1));
	uintptr_t mod = nodePhys & alignmentMask;
	nPagesRequired += mod / OBOS_PAGE_SIZE;
	if (nPagesRequiredP)
		*nPagesRequiredP = nPagesRequired;
	return node->nPages >= nPagesRequired;
}
#define MAP_TO_HHDM(addr, type) ((type*)(MmS_MapVirtFromPhys((uintptr_t)(addr))))
#define UNMAP_FROM_HHDM(addr) (MmS_UnmapVirtFromPhys((void*)(addr)))
OBOS_NO_KASAN static uintptr_t allocate(size_t nPages, size_t alignmentPages, obos_status *status, struct freelist_node** const head, struct freelist_node** const tail)
{
	if (!nPages)
	{
		*status = OBOS_STATUS_INVALID_ARGUMENT;
		return 0;
	}
	if (!alignmentPages)
		alignmentPages = 1;
	if (__builtin_popcount(alignmentPages) > 1)
	{
		if (status)
			*status = OBOS_STATUS_INVALID_ARGUMENT;
		return 0;
	}
	if (!(*head))
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		return 0;
	}
	uintptr_t phys = 0;
	if (nPages % alignmentPages)
		nPages += (alignmentPages - (nPages % alignmentPages));
	size_t alignmentMask = alignmentPages*OBOS_PAGE_SIZE-1;
	size_t nPagesRequired = 0;
	irql oldIrql = Core_SpinlockAcquireExplicit(&lock, IRQL_DISPATCH, true);
	struct freelist_node* node = MAP_TO_HHDM((*head), struct freelist_node);
	while (UNMAP_FROM_HHDM(node) && !IsRegionSufficient(node, nPages, alignmentMask, &nPagesRequired))
		node = MAP_TO_HHDM(node->next, struct freelist_node);
	if (!UNMAP_FROM_HHDM(node))
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		Core_SpinlockRelease(&lock, oldIrql);
		return 0;
	}
	OBOS_ASSERT(node->nPages >= nPagesRequired);
	node->nPages -= nPagesRequired;
	Mm_TotalPhysicalPagesUsed += nPagesRequired;
	if (!node->nPages)
	{
		if (node->next)
			MAP_TO_HHDM(node->next, struct freelist_node)->prev = node->prev;
		if (node->prev)
			MAP_TO_HHDM(node->prev, struct freelist_node)->next = node->next;
		if ((uintptr_t)(*head) == UNMAP_FROM_HHDM(node))
			(*head) = node->next;
		if ((uintptr_t)(*tail) == UNMAP_FROM_HHDM(node))
			(*tail) = node->prev;
		s_nNodes--;
	}
	phys = ((uintptr_t)node) + node->nPages * OBOS_PAGE_SIZE;
	phys = UNMAP_FROM_HHDM(phys);
	if (!node->nPages)
		memzero(node, sizeof(*node));
	if (status)
		*status = OBOS_STATUS_SUCCESS;
	Core_SpinlockRelease(&lock, oldIrql);
	OBOS_ASSERT(phys);
	OBOS_ASSERT(phys < Mm_PhysicalMemoryBoundaries);
	return phys;
}
OBOS_NO_KASAN static uintptr_t allocate_phys_or_fail(size_t nPages, size_t alignmentPages, obos_status *status)
{
	uintptr_t res = allocate(nPages, alignmentPages, status, &s_head, &s_tail);
	if (res)
		return res;
#if OBOS_ARCHITECTURE_BITS == 64
	if (status)
		*status = OBOS_STATUS_SUCCESS;
	return allocate(nPages, alignmentPages, status, &s_head32, &s_tail32);
#else
	return 0;
#endif
}
OBOS_NO_KASAN uintptr_t Mm_AllocatePhysicalPages(size_t nPages, size_t alignmentPages, obos_status *status)
{
	uintptr_t res = allocate_phys_or_fail(nPages, alignmentPages, status);
	if (res)
		return res;
	if (!Mm_IsInitialized())
		return 0; // oof.
	if (nPages > (OBOS_HUGE_PAGE_SIZE / OBOS_PAGE_SIZE))
	{
		if (status)
			*status = OBOS_STATUS_UNIMPLEMENTED;
		return 0;
	}
	// take a standby page large enough.
	start_again:
	(void)0;
	irql oldIrql = Mm_TakeSwapLock();
	page* node = LIST_GET_HEAD(phys_page_list, &Mm_StandbyPageList);
	while (node)
	{
		if (nPages == 1)
			break;
		if (node->flags & PHYS_PAGE_HUGE_PAGE && nPages != 1)
			break;
		node = LIST_GET_NEXT(phys_page_list, &Mm_StandbyPageList, node);
	}
	if (node)
	{
		// Remove from standby.
		LIST_REMOVE(phys_page_list, &Mm_StandbyPageList, node);
		res = node->phys;
		// We need to update page mappings.
		for (page_node *curr = node->virt_pages.head; curr; )
		{
			curr->data->prot.is_swap_phys = true;
			curr->data->phys = node->swap_id;
			curr = curr->next;
		}
		RB_REMOVE(phys_page_tree, &Mm_PhysicalPages, node);
        Mm_Allocator->Free(Mm_Allocator, node, sizeof(*node));
	}
	else
	{
		// OOM!
		Mm_ReleaseSwapLock(oldIrql);
		Mm_WakePageWriter(true);
		goto start_again;
	} 
	Mm_ReleaseSwapLock(oldIrql);
	return res;
}
OBOS_NO_KASAN uintptr_t Mm_AllocatePhysicalPages32(size_t nPages, size_t alignmentPages, obos_status *status)
{
#if OBOS_ARCHITECTURE_BITS == 64
	return allocate(nPages, alignmentPages, status, &s_head32, &s_tail32);
#else
	return Mm_AllocatePhysicalPages(nPages, alignmentPages, status);
#endif
}
OBOS_NO_KASAN static obos_status free(uintptr_t addr, size_t nPages, struct freelist_node** const head, struct freelist_node** const tail)
{
	if (!nPages)
		return OBOS_STATUS_SUCCESS; // nothing freed, no-op.
	irql oldIrql = Core_SpinlockAcquireExplicit(&lock, IRQL_DISPATCH, true);
	struct freelist_node* node = MAP_TO_HHDM(addr, struct freelist_node);
	node->nPages = nPages;
	if ((*tail))
		MAP_TO_HHDM((*tail), struct freelist_node)->next = (struct freelist_node*)UNMAP_FROM_HHDM(node);
	if (!(*head))
		(*head) = (struct freelist_node*)UNMAP_FROM_HHDM(node);
	node->prev = (*tail);
	(*tail) = (struct freelist_node*)UNMAP_FROM_HHDM(node);
	s_nNodes--;
	Mm_TotalPhysicalPagesUsed -= nPages;
	Core_SpinlockRelease(&lock, oldIrql);
	return OBOS_STATUS_SUCCESS;
}
OBOS_NO_KASAN obos_status Mm_FreePhysicalPages(uintptr_t addr, size_t nPages)
{
	OBOS_ASSERT(addr);
	OBOS_ASSERT(!(addr % OBOS_PAGE_SIZE));
	OBOS_ASSERT(addr < Mm_PhysicalMemoryBoundaries);
	addr -= (addr%OBOS_PAGE_SIZE);
	if (!addr)
		return OBOS_STATUS_INVALID_ARGUMENT;
#if OBOS_ARCHITECTURE_BITS == 64
	if (addr < 0xffffffff)
	{
		if ((addr + (nPages*OBOS_PAGE_SIZE)) >= 0xffffffff)
		{
			size_t pages = ((addr + (nPages*OBOS_PAGE_SIZE)) & ~0xffffffff) / OBOS_PAGE_SIZE;
			free(0x100000000, pages, &s_head, &s_tail);
			nPages -= pages;
			OBOS_ASSERT((addr + (nPages*OBOS_PAGE_SIZE)) < 0xffffffff);
		}
		if (nPages)
			free(addr, nPages, &s_head32, &s_tail32);
	}
	else
		free(addr, nPages, &s_head, &s_tail);
#else
	free(addr, nPages, &s_head, &s_tail);
#endif
	// OBOS_Debug("%s: Marking physical memory region at 0x%p-0x%p as free.\n", __func__, addr, addr+nPages*OBOS_PAGE_SIZE);
	return OBOS_STATUS_SUCCESS;
}