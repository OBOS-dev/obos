/*
	oboskrnl/arch/x86_64/pmm/alloc.h

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <arch/x86_64/pmm/alloc.h>

#include <limine/limine.h>

#include <locks/spinlock.h>

#include <irq/irql.h>

namespace obos
{
	volatile limine_memmap_request mmap_request = {
		.id = LIMINE_MEMMAP_REQUEST,
		.revision = 1
	};
	extern volatile limine_hhdm_request hhdm_offset;
#define MAP_TO_HHDM(addr) (hhdm_offset.response->offset + (uintptr_t)(addr))
	struct MemoryNode
	{
		MemoryNode* next, * prev;
		size_t nPages;
	};
	MemoryNode* g_memoryHead, * g_memoryTail;
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
		g_pmmLock.Unlock();
	}

	uintptr_t AllocatePhysicalPages(size_t nPages)
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
		while (node->nPages < nPages)
		{
			node = node->next;
			if (!node)
				return 0; // Not enough physical memory to satisfy request of nPages.
			nodePhys = node;
			node = (MemoryNode*)MAP_TO_HHDM((uintptr_t*)node);
		}
		node->nPages -= nPages;
		if (!node->nPages)
		{
			// This node has no free pages after this allocation, so it should be removed.
			MemoryNode* next = node->next, * prev = node->prev;
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
		ret = ret + node->nPages * 4096;
		if (oldIRQL != 0xff)
			LowerIRQL(oldIRQL);
		g_pmmLock.Unlock();
		return ret;
	}
	void FreePhysicalPages(uintptr_t addr, size_t nPages)
	{
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
}