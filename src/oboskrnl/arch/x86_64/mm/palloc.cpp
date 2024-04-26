/*
	oboskrnl/arch/x86_64/mm/palloc.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/mm/map.h>

#include <limine/limine.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

namespace obos
{
#define MAP_TO_HHDM(addr) (hhdm_offset.response->offset + (uintptr_t)(addr))
#define CMAP_TO_HHDM(addr) reinterpret_cast<decltype(addr)>(hhdm_offset.response->offset + (uintptr_t)(addr))
#define REMOVE_FROM_HHDM(addr) ((uintptr_t)(addr) - hhdm_offset.response->offset)
	volatile limine_memmap_request mmap_request = {
		.id = LIMINE_MEMMAP_REQUEST,
		.revision = 1
	};
	extern volatile limine_hhdm_request hhdm_offset;
	uintptr_t hhdm_limit = 0;
	struct MemoryNode
	{
		MemoryNode *next, *prev;
		size_t nPages;
	};
	MemoryNode *g_memoryHead, *g_memoryTail;
	size_t g_nMemoryNodes;
	size_t g_nPhysPagesUsed = 0;
	bool g_pmmInitialized = false;
	locks::SpinLock g_pmmLock;
	static uintptr_t CalculateHHDMLimit()
	{
		auto lastMMAPEntry = mmap_request.response->entries[mmap_request.response->entry_count - 1];
		uintptr_t limit = hhdm_offset.response->offset + (uintptr_t)lastMMAPEntry->base + (lastMMAPEntry->length / 4096) * 4096;
		vmm::page_descriptor pd{};
		arch::get_page_descriptor((vmm::Context*)nullptr, (void*)limit, pd);
		size_t i = 1;
		while (!pd.present)
		{
			lastMMAPEntry = mmap_request.response->entries[mmap_request.response->entry_count - ++i];
			if (lastMMAPEntry < *mmap_request.response->entries)
			{
				limit = hhdm_offset.response->offset;
				break;
			}
			limit = hhdm_offset.response->offset + (uintptr_t)lastMMAPEntry->base + (lastMMAPEntry->length / 4096) * 4096;
			arch::get_page_descriptor((vmm::Context*)nullptr, (void*)(limit - 0x1000), pd);
		}
		// Round the limit up.
		limit += 0x200000;
		limit &= ~(0x200000 - 1);
		return limit;
	}
	namespace arch
	{
		uintptr_t getHHDMLimit()
		{
			if (g_pmmInitialized)
				return hhdm_limit;
			return CalculateHHDMLimit();
		}
	}
	void InitializePMM()
	{
		if (g_pmmInitialized)
			return;
		new (&g_pmmLock) locks::SpinLock{};
		g_pmmLock.Lock();
		for (size_t i = 0; i < mmap_request.response->entry_count; i++)
		{
			if (mmap_request.response->entries[i]->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE || mmap_request.response->entries[i]->type == LIMINE_MEMMAP_KERNEL_AND_MODULES)
				g_nPhysPagesUsed += ((mmap_request.response->entries[i]->length + 0xfff) & ~0xfff) >> 12;
			if (mmap_request.response->entries[i]->type != LIMINE_MEMMAP_USABLE)
				continue;
			uintptr_t base = mmap_request.response->entries[i]->base;
			if (base < 0x1000)
				base = 0x1000;
			if (base & 0xfff)
				base = (base + 0xfff) & 0xfff;
			size_t size = mmap_request.response->entries[i]->length & ~0xfff;
			g_nMemoryNodes++;
			MemoryNode* newNode = (MemoryNode*)MAP_TO_HHDM(base);
			MemoryNode* newNodePhys = (MemoryNode*)base;
			if (g_memoryTail)
				((MemoryNode*)MAP_TO_HHDM(g_memoryTail))->next = newNodePhys;
			if (!g_memoryHead)
				g_memoryHead = newNodePhys;
			newNode->prev = g_memoryTail;
			newNode->next = nullptr;
			newNode->nPages = size / 0x1000;
			g_memoryTail = newNodePhys;
		}
		g_pmmLock.Unlock();
		hhdm_limit = CalculateHHDMLimit();
		g_pmmInitialized = true;
	}

	uintptr_t AllocatePhysicalPages(size_t nPages, bool align2MIB)
	{
		if (!g_nMemoryNodes)
			logger::panic(nullptr, "No more available physical memory left.\n");
		uint8_t oldIRQL = 0xff;
		g_pmmLock.Lock();
		if (GetIRQL() < 2)
			RaiseIRQL(2, &oldIRQL);
		MemoryNode* node = (MemoryNode*)MAP_TO_HHDM((uintptr_t*)g_memoryHead);
		MemoryNode* nodePhys = g_memoryHead;
		uintptr_t ret = (uintptr_t)g_memoryHead;
		if (align2MIB && nPages & 0x1ff)
			nPages = (nPages + 0x1ff) & ~0x1ff;
		size_t nPagesRequired = nPages + (align2MIB ? (node->nPages & 0x1ff) : 0);
		if (align2MIB && ((uintptr_t)nodePhys & 0x1f'ffff))
			nPagesRequired += ((uintptr_t)nodePhys & 0x1f'ffff) / 4096;
		while (node->nPages < nPagesRequired)
		{
			node = node->next;
			if (!node)
			{
				if (oldIRQL != 0xff)
					LowerIRQL(oldIRQL);
				return 0; // Not enough physical memory to satisfy request of nPages.
			}
			nodePhys = node;
			node = (MemoryNode*)MAP_TO_HHDM((uintptr_t*)node);
			nPagesRequired = nPages;
			if (align2MIB)
			{
				nPagesRequired += (node->nPages & 0x1ff);
				nPagesRequired += ((uintptr_t)nodePhys & 0x1f'ffff) / 4096;
			}
		}
		node->nPages -= nPagesRequired;
		if (!node->nPages)
		{
			// This node has no free pages after this allocation, so it should be removed.
			MemoryNode* next = node->next, *prev = node->prev;
			if (next)
				((MemoryNode*)MAP_TO_HHDM((uintptr_t*)next))->prev = prev;
			if (prev)
				((MemoryNode*)MAP_TO_HHDM((uintptr_t*)prev))->next = next;
			if (g_memoryHead == nodePhys)
				g_memoryHead = next;
			if (g_memoryTail == nodePhys)
				g_memoryTail = prev;
			node->next = nullptr;
			node->prev = nullptr;
		}
		ret = (uintptr_t)nodePhys + node->nPages * 4096;
		if (oldIRQL != 0xff)
			LowerIRQL(oldIRQL);
		g_pmmLock.Unlock();
		__atomic_add_fetch(&g_nPhysPagesUsed, nPages, __ATOMIC_SEQ_CST);
		return ret;
	}
	void FreePhysicalPages(uintptr_t addr, size_t nPages)
	{
		OBOS_ASSERTP(addr != 0, "Attempt free of physical address zero.\n");
		addr &= ~0xfff;
		OBOS_ASSERTP((uintptr_t)addr != hhdm_limit, "");
		g_pmmLock.Lock();
		MemoryNode* node = (MemoryNode*)MAP_TO_HHDM((uintptr_t*)addr);
		MemoryNode* nodePhys = (MemoryNode*)addr;
		OBOS_ASSERTP((uintptr_t)g_memoryTail != hhdm_limit, "");
		if (g_memoryTail)
			CMAP_TO_HHDM(g_memoryTail)->next = nodePhys;
		if (!g_memoryHead)
			g_memoryHead = nodePhys;
		node->next = nullptr;
		node->prev = g_memoryTail;
		g_memoryTail = nodePhys;
		node->nPages = nPages;
		g_nMemoryNodes++;
		g_pmmLock.Unlock();
		__atomic_sub_fetch(&g_nPhysPagesUsed, nPages, __ATOMIC_SEQ_CST);
	}
	static void swapNodes(MemoryNode* node, MemoryNode* nodePhys, MemoryNode* with, MemoryNode* withPhys)
	{
		if (!node || !with)
			return;
		struct MemoryNode* aPrev = node->prev;
		struct MemoryNode* aNext = node->next;
		struct MemoryNode* bPrev = with->prev;
		struct MemoryNode* bNext = with->next;
		if (aPrev == withPhys)
		{
			// Assuming the nodes are valid, bNext == node
			node->prev = bPrev;
			node->next = withPhys;
			with->prev = nodePhys;
			with->next = aNext;
			if (bPrev) CMAP_TO_HHDM(bPrev)->next = nodePhys;
			if (aNext) CMAP_TO_HHDM(aNext)->prev = withPhys;
		}
		else if (aNext == withPhys)
		{
			// Assuming the nodes are valid, bPrev == node
			node->prev = withPhys;
			node->next = bNext;
			with->prev = aPrev;
			with->next = nodePhys;
			if (bNext) CMAP_TO_HHDM(bNext)->prev = nodePhys;
			if (aPrev) CMAP_TO_HHDM(aPrev)->next = withPhys;
		}
		else
		{
			node->prev = bPrev;
			node->next = bNext;
			with->prev = aPrev;
			with->next = aNext;
			if (aPrev) CMAP_TO_HHDM(aPrev)->next = withPhys;
			if (aNext) CMAP_TO_HHDM(aPrev)->prev = withPhys;
			if (bPrev) CMAP_TO_HHDM(bPrev)->next = nodePhys;
			if (bNext) CMAP_TO_HHDM(bNext)->prev = nodePhys;
		}
		if (g_memoryHead == withPhys)
			g_memoryHead = nodePhys;
		else if (g_memoryHead == nodePhys)
			g_memoryHead = withPhys;
		if (g_memoryTail == withPhys)
			g_memoryTail = nodePhys;
		else if (g_memoryTail == nodePhys)
			g_memoryTail = withPhys;
	}
	void OptimizePMMFreeList()
	{
		g_pmmLock.Lock();
		MemoryNode* currentNode = CMAP_TO_HHDM(g_memoryHead);
		uintptr_t currentNodePhys = (uintptr_t)currentNode - hhdm_offset.response->offset;
		uintptr_t stepNodePhys = 0;
		bool swapped = false;
		// Sort the nodes into lowest address to highest address using bubble sort.
		do
		{
			swapped = false;
			currentNode = CMAP_TO_HHDM(g_memoryHead);
			currentNodePhys = (uintptr_t)currentNode - hhdm_offset.response->offset;
			if (!currentNodePhys)
				break;

			while ((uintptr_t)currentNode->next != stepNodePhys)
			{
				if (!currentNodePhys)
					break;
				if (currentNodePhys > (uintptr_t)currentNode->next)
				{
					// Swap the nodes.
					if (!currentNode->next)
						break;
					MemoryNode* nextNode = CMAP_TO_HHDM(currentNode->next);
					swapNodes(currentNode, (MemoryNode*)currentNodePhys, nextNode, currentNode->next);
					swapped = true;
				}
				currentNode = (MemoryNode*)MAP_TO_HHDM(currentNodePhys = (uintptr_t)currentNode->next);
				if (!currentNodePhys)
					break;
			}

			if (!currentNodePhys)
				break;
			stepNodePhys = currentNodePhys;
		} while (swapped);
		currentNode = (MemoryNode*)MAP_TO_HHDM(((MemoryNode*)MAP_TO_HHDM(g_memoryHead))->next);
		currentNodePhys = (uintptr_t)currentNode - hhdm_offset.response->offset;
		while (currentNodePhys)
		{
			uintptr_t previousNodePhys = (uintptr_t)currentNode->prev;
			MemoryNode* previousNode = (MemoryNode*)MAP_TO_HHDM(currentNode->prev);
			OBOS_ASSERTP(previousNodePhys != 0, "");
			uintptr_t nextNodePhys = (uintptr_t)currentNode->next;
			MemoryNode* nextNode = (MemoryNode*)MAP_TO_HHDM(currentNode->next);
			// Two blocks that are continuous but in separate nodes.
			if ((previousNodePhys + previousNode->nPages * 4096) == currentNodePhys)
			{
				// Combine them.
				previousNode->nPages += currentNode->nPages;
				if (nextNodePhys)
					nextNode->prev = (MemoryNode*)previousNodePhys;
				// It's guaranteed that previousNodePhys != 0
				previousNode->next = (MemoryNode*)nextNodePhys;
				// Because of that guarantee, currentNode cannot be equal to g_memoryHead
				/*if (currentNode == g_memoryHead)
					g_memoryHead = currentNode->next;*/
				if (currentNodePhys == (uintptr_t)g_memoryTail)
					g_memoryTail = (MemoryNode*)previousNodePhys;
				g_nMemoryNodes--;
			}

			currentNode = (MemoryNode*)MAP_TO_HHDM(currentNodePhys = (uintptr_t)currentNode->next);
		}
		g_pmmLock.Unlock();
	}

	void* MapToHHDM(uintptr_t phys)
	{
		return (void*)MAP_TO_HHDM(phys);
	}
}