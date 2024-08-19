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

struct freelist_node
{
#if OBOS_KASAN_ENABLED
	uint8_t poison[64];
#endif
	size_t nPages;
	struct freelist_node *next, *prev;
};
static struct freelist_node *s_head;
static struct freelist_node *s_tail;
static size_t s_nNodes;
size_t Mm_TotalPhysicalPages;
size_t Mm_TotalPhysicalPagesUsed;
size_t Mm_UsablePhysicalPages;
uintptr_t Mm_PhysicalMemoryBoundaries;
thread_list Mm_ThreadsAwaitingPhysicalMemory;
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
OBOS_NO_KASAN uintptr_t Mm_AllocatePhysicalPages(size_t nPages, size_t alignmentPages, obos_status *status)
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
		*status = OBOS_STATUS_INVALID_ARGUMENT;
		return 0;
	}
	if (!s_head)
		OBOS_Panic(OBOS_PANIC_NO_MEMORY, "No more avaliable physical memory!\n");
	uintptr_t phys = 0;
	if (nPages % alignmentPages)
		nPages += (alignmentPages - (nPages % alignmentPages));
	size_t alignmentMask = alignmentPages*OBOS_PAGE_SIZE-1;
	size_t nPagesRequired = 0;
	irql oldIrql = Core_SpinlockAcquireExplicit(&lock, IRQL_MASKED, true);
	struct freelist_node* node = MAP_TO_HHDM(s_head, struct freelist_node);
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
		if ((uintptr_t)s_head == UNMAP_FROM_HHDM(node))
			s_head = node->next;
		if ((uintptr_t)s_tail == UNMAP_FROM_HHDM(node))
			s_tail = node->prev;
		s_nNodes--;
		node->next = nullptr;
		node->prev = nullptr;
	}
	phys = ((uintptr_t)node) + node->nPages * OBOS_PAGE_SIZE;
	phys = UNMAP_FROM_HHDM(phys);
	if (status)
		*status = OBOS_STATUS_SUCCESS;
	Core_SpinlockRelease(&lock, oldIrql);
	OBOS_ASSERT(phys);
	OBOS_ASSERT(phys < Mm_PhysicalMemoryBoundaries);
	return phys;
}
OBOS_NO_KASAN obos_status Mm_FreePhysicalPages(uintptr_t addr, size_t nPages)
{
	OBOS_ASSERT(addr);
	OBOS_ASSERT(!(addr % OBOS_PAGE_SIZE));
	OBOS_ASSERT(addr < Mm_PhysicalMemoryBoundaries);
	addr -= (addr%OBOS_PAGE_SIZE);
	if (!addr)
		return OBOS_STATUS_INVALID_ARGUMENT;
	irql oldIrql = Core_SpinlockAcquireExplicit(&lock, IRQL_MASKED, true);
	struct freelist_node* node = MAP_TO_HHDM(addr, struct freelist_node);
#if OBOS_KASAN_ENABLED
	memset(node->poison, OBOS_ASANPoisonValues[ASAN_POISON_ALLOCATED], sizeof(node->poison));
#endif
	node->nPages = nPages;
	if (s_tail)
		MAP_TO_HHDM(s_tail, struct freelist_node)->next = (struct freelist_node*)addr;
	if (!s_head)
		s_head = (struct freelist_node*)addr;
	node->prev = s_tail;
	Mm_TotalPhysicalPagesUsed -= nPages;
	s_tail = (struct freelist_node*)addr;
	Core_SpinlockRelease(&lock, oldIrql);
	// OBOS_Debug("%s: Marking physical memory region at 0x%p-0x%p as free.\n", __func__, addr, addr+nPages*OBOS_PAGE_SIZE);
	size_t nMemoryLeft = nPages * OBOS_PAGE_SIZE;
	for (thread_node* node = Mm_ThreadsAwaitingPhysicalMemory.head; node; )
	{
		thread_node* next = node->next;
		thread* const curr = node->data;
		if (nMemoryLeft >= curr->nBytesWaitingFor)
		{
			nMemoryLeft -= curr->nBytesWaitingFor;
			CoreH_ThreadListRemove(&Mm_ThreadsAwaitingPhysicalMemory, node);
			CoreH_ThreadReadyNode(curr, curr->snode);
		}

		node = next;
	}
	return OBOS_STATUS_SUCCESS;
}