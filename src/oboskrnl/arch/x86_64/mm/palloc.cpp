/*
	oboskrnl/arch/x86_64/mm/palloc.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <arch/x86_64/mm/palloc.h>

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
	bool g_pmmInitialized = false;
	locks::SpinLock g_pmmLock;
	void InitializePMM()
	{
		if (g_pmmInitialized)
			return;
		new (&g_pmmLock) locks::SpinLock{};
		g_pmmLock.Lock();
		for (size_t i = 0; i < mmap_request.response->entry_count; i++)
		{
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
		auto& lastMMAPEntry =
			mmap_request.response->entries[mmap_request.response->entry_count - 2]
			;
		hhdm_limit = hhdm_offset.response->offset + (uintptr_t)lastMMAPEntry->base + (lastMMAPEntry->length / 4096) * 4096;
		g_pmmLock.Unlock();
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
				return 0; // Not enough physical memory to satisfy request of nPages.
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
		return ret;
	}
	void FreePhysicalPages(uintptr_t addr, size_t nPages)
	{
		OBOS_ASSERTP(addr != 0, "Attempt free of physical address zero.\n");
		addr &= ~0xfff;
		g_pmmLock.Lock();
		MemoryNode* node = (MemoryNode*)MAP_TO_HHDM((uintptr_t*)addr);
		MemoryNode* nodePhys = (MemoryNode*)addr;
		if (g_memoryTail)
			((MemoryNode*)MAP_TO_HHDM((uintptr_t*)g_memoryTail))->next = nodePhys;
		if (!g_memoryHead)
			g_memoryHead = nodePhys;
		node->next = nullptr;
		node->prev = g_memoryTail;
		g_memoryTail = nodePhys;
		node->nPages = nPages;
		g_nMemoryNodes++;
		g_pmmLock.Unlock();
	}
	void OptimizePMMFreeList()
	{
		g_pmmLock.Lock();
		MemoryNode* currentNode = CMAP_TO_HHDM(g_memoryHead);
		uintptr_t currentNodePhys = (uintptr_t)currentNode - hhdm_offset.response->offset;
		MemoryNode* stepNode = nullptr;
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
					MemoryNode temp = *currentNode;
					currentNode->next = nextNode->next;
					currentNode->prev = nextNode->prev;
					currentNode->nPages = nextNode->nPages;
					nextNode->next = temp.next;
					nextNode->prev = temp.prev;
					nextNode->nPages = temp.nPages;
					swapped = true;
				}
				currentNode = (MemoryNode*)MAP_TO_HHDM(currentNodePhys = (uintptr_t)currentNode->next);
			}

			if (!currentNodePhys)
				break;
			stepNode = currentNode;
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
			// Two blocks that are continuous but in seperate nodes.
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
					g_memoryHead = ;*/
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