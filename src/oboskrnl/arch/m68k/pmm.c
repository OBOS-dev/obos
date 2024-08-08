/*
	oboskrnl/arch/x86_64/pmm.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/m68k/pmm.h>

#include <arch/m68k/loader/Limine.h>
#include <stdint.h>

extern volatile struct limine_memmap_request Arch_MemmapRequest;
extern volatile struct limine_hhdm_request Arch_HHDMRequest;
struct freelist_node
{
	size_t nPages;
	struct freelist_node *next, *prev;
};
static struct freelist_node *s_head;
static struct freelist_node *s_tail;
static size_t s_nNodes;
size_t Arch_TotalPhysicalPages = 0;
size_t Arch_TotalPhysicalPagesUsed = 0;
size_t Arch_UsablePhysicalPages = 0;
uintptr_t Arch_PhysicalMemoryBoundaries;
obos_status Arch_InitializePMM()
{
	size_t nEntries = Arch_MemmapRequest.response->entry_count;
	if (!nEntries)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No memory map entries.\n");
	for (size_t i = 0; i < nEntries; i++)
	{
		struct limine_memmap_entry* entry = Arch_MemmapRequest.response->entries[i];
		uintptr_t phys = entry->base;
		size_t nPages = entry->length / 0x1000;
		if (phys & 0xfff)
		{
			phys = (phys + 0xfff) & ~0xfff;
			nPages--;
		}
		if (phys == 0x0)
		{
			phys = 0x1000;
			nPages--;
		}
		Arch_TotalPhysicalPages += nPages;
		if ((phys + nPages * 0x1000) > Arch_PhysicalMemoryBoundaries)
			Arch_PhysicalMemoryBoundaries = (phys + nPages * 0x1000);
		if (entry->type != LIMINE_MEMMAP_USABLE)
			continue;
		Arch_UsablePhysicalPages += nPages;
		struct freelist_node* node = (struct freelist_node*)((uintptr_t)Arch_HHDMRequest.response->offset + phys);
		memzero(node, sizeof(*node));
		node->nPages = nPages;
		if (s_tail)
			((struct freelist_node*)((uintptr_t)Arch_HHDMRequest.response->offset + (uintptr_t)s_tail))->next = (struct freelist_node*)phys;
		if (!s_head)
			s_head = (struct freelist_node*)phys;
		node->prev = s_tail;
		s_tail = (struct freelist_node*)phys;
		s_nNodes++;
	}
	if (Arch_PhysicalMemoryBoundaries & 0xfff)
		Arch_PhysicalMemoryBoundaries = (Arch_PhysicalMemoryBoundaries + 0xfff) & ~0xfff;
	return OBOS_STATUS_SUCCESS;
}
static bool IsRegionSufficient(struct freelist_node* node, size_t nPages, size_t alignmentMask, size_t *nPagesRequiredP)
{
	size_t nPagesRequired = nPages;
	uintptr_t nodePhys = (uintptr_t)node - Arch_HHDMRequest.response->offset;
	nPagesRequired += (node->nPages & ((alignmentMask + 1) / 0x1000 - 1));
	uintptr_t mod = nodePhys & alignmentMask;
	nPagesRequired += mod / 4096;
	if (nPagesRequiredP)
		*nPagesRequiredP = nPagesRequired;
	return node->nPages >= nPagesRequired;
}
#define MAP_TO_HHDM(addr, type) ((type*)((uintptr_t)Arch_HHDMRequest.response->offset + (uintptr_t)(addr)))
#define UNMAP_FROM_HHDM(addr) ((uintptr_t)(addr) - (uintptr_t)Arch_HHDMRequest.response->offset)
OBOS_NO_UBSAN OBOS_NO_KASAN uintptr_t Arch_AllocatePhysicalPages(size_t nPages, size_t alignmentPages, obos_status *status)
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
	size_t alignmentMask = alignmentPages*0x1000-1;
	size_t nPagesRequired = 0;
	struct freelist_node* node = MAP_TO_HHDM(s_head, struct freelist_node);
	while (UNMAP_FROM_HHDM(node) && !IsRegionSufficient(node, nPages, alignmentMask, &nPagesRequired))
		node = MAP_TO_HHDM(node->next, struct freelist_node);
	if (!UNMAP_FROM_HHDM(node))
	{
		if (status)
			*status = OBOS_STATUS_NOT_ENOUGH_MEMORY;
		return 0;
	}
	OBOS_ASSERT(node->nPages >= nPagesRequired);
	node->nPages -= nPagesRequired;
	Arch_TotalPhysicalPagesUsed += nPagesRequired;
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
	phys = (uintptr_t)node + node->nPages * 0x1000;
	phys -= Arch_HHDMRequest.response->offset;
	if (status)
		*status = OBOS_STATUS_SUCCESS;
	return phys;
}
OBOS_NO_UBSAN OBOS_NO_KASAN void Arch_FreePhysicalPages(uintptr_t addr, size_t nPages)
{
	OBOS_ASSERT(addr);
	OBOS_ASSERT(!(addr & 0xfff));
	addr &= ~0xfff;
	if (!addr)
		return;
	struct freelist_node* node = (struct freelist_node*)(uintptr_t)(Arch_HHDMRequest.response->offset+ addr);
	node->nPages = nPages;
	if (s_tail)
		MAP_TO_HHDM(s_tail, struct freelist_node)->next = (struct freelist_node*)addr;
	if (!s_head)
		s_head = (struct freelist_node*)addr;
	node->prev = s_tail;
	Arch_TotalPhysicalPagesUsed -= nPages;
	s_tail = (struct freelist_node*)addr;
}
OBOS_NO_UBSAN OBOS_NO_KASAN uintptr_t OBOSS_AllocatePhysicalPages(size_t nPages, size_t alignment, obos_status* status)
{
	return Arch_AllocatePhysicalPages(nPages, alignment, status);
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status OBOSS_FreePhysicalPages(uintptr_t base, size_t nPages)
{
	base &= ~0xfff;
	if (!base)
		return OBOS_STATUS_INVALID_ARGUMENT;
	Arch_FreePhysicalPages(base, nPages);
	return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN OBOS_NO_KASAN void* Arch_MapToHHDM(uintptr_t phys)
{
	return MAP_TO_HHDM(phys, void);
}
OBOS_NO_UBSAN OBOS_NO_KASAN uintptr_t Arch_UnmapFromHHDM(void* virt)
{
	return UNMAP_FROM_HHDM(virt);
}