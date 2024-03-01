/*
	oboskrnl/allocators/slab.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/slab.h>
#include <allocators/slab_structs.h>

#include <vmm/map.h>
#include "slab_structs.h"

namespace obos
{
	namespace allocators
	{
#define ROUND_UP(n, to) ((to) != 0 ? ((n) / (to) + 1) * (to) : (n))
#define ROUND_UP_COND(n, to) ((to) != 0 ? ((n) / (to) + (((n) % (to)) != 0)) * (to) : (n))
		bool SlabAllocator::Initialize(void* allocBase, size_t allocSize, size_t initialNodeCount, size_t padding)
		{
			if (!allocBase)
				return false;
			if (!initialNodeCount)
				initialNodeCount = OBOS_INITIAL_SLAB_COUNT;
			allocSize = ROUND_UP_COND(allocSize, padding);
			size_t sizeNeeded = allocSize + sizeof(SlabNode);
			m_stride = ROUND_UP_COND(sizeNeeded, padding);
			size_t regionSize = m_stride * initialNodeCount;
			regionSize = ROUND_UP_COND(regionSize, padding);
			m_base = vmm::RawAllocate((void*)allocBase, regionSize, 0, vmm::PROT_NO_DEMAND_PAGE);
			if (!m_base)
				return false;
			memzero(m_base, regionSize);
			m_regionSize = regionSize;
			m_allocationSize = allocSize;
			m_padding = padding;
			// Register all free nodes.
			size_t i = 0;
			for (SlabNode* cur = (SlabNode*)m_base; i < initialNodeCount; cur = (SlabNode*)((uintptr_t)cur + m_stride), i++)
			{
				cur->size = allocSize;
				cur->data = (char*)ROUND_UP(((uintptr_t)(cur + 1) - sizeof(uintptr_t)), padding);
				m_freeNodes.Append(cur);
			}
			return true;
		}

		static void* AllocateNode(SlabList& freeList, SlabList& allocatedList, SlabNode* node, size_t sz, size_t padding)
		{
			size_t requiredSize = ROUND_UP_COND(sz + sizeof(SlabNode), padding);
			if (node->size < requiredSize)
				return nullptr;
			node->size -= requiredSize;
			if (!node->size)
				freeList.Remove(node);
			SlabNode* newNode = (SlabNode*)(node->data + node->size);
			memzero(newNode, sizeof(*newNode));
			newNode->size = sz;
			newNode->data = (char*)ROUND_UP(((uintptr_t)(newNode + 1) - sizeof(uintptr_t)), padding);
			allocatedList.Append(newNode);
			return newNode->data;
		}
		void* SlabAllocator::Allocate(size_t size)
		{
			void* ret = nullptr;
			size *= m_allocationSize;
			size = ROUND_UP_COND(size, m_padding);
			m_lock.Lock();
			if (!m_freeNodes.nNodes)
			{
				m_lock.Unlock();
				return nullptr;
			}
			ImplOptimizeAllocator();
			for (SlabNode* node = m_freeNodes.tail; node;)
			{
				if ((ret = AllocateNode(m_freeNodes, m_allocatedNodes, node, size, m_padding)))
					break;
				node = node->prev;
			}
			m_lock.Unlock();
			return ret;
		}

		static SlabNode* LookForNode(SlabList& list, void* addr)
		{
			for (SlabNode* node = list.head; node;)
			{
				if ((addr >= node->data) && (addr < (node->data + node->size)))
					return node;
				node = node->next;
			}
			return nullptr;
		}

		void* SlabAllocator::ReAllocate(void* /*base*/, size_t /*newSize*/)
		{
			TODO("Implement SlabAllocator::ReAllocate.")
			OBOS_ASSERTP(!"unimplemented", "Function is unimplemented.");
			return nullptr;
		}

		void SlabAllocator::Free(void* base, size_t)
		{
			SlabNode* node = LookForNode(m_allocatedNodes, base);
			if (!node)
				return;
			m_lock.Lock();
			memzero(node->data, node->size);
			m_allocatedNodes.Remove(node);
			node->next = nullptr;
			node->prev = nullptr;
			m_freeNodes.Append(node);
			m_lock.Unlock();
		}

		size_t SlabAllocator::QueryObjectSize(void* base)
		{
			if (base < m_base || base >= ((char*)m_base + m_regionSize))
				return SIZE_MAX;
			SlabNode* node = LookForNode(m_allocatedNodes, base);
			if (!node)
				return SIZE_MAX;
			return node->size / m_allocationSize;
		}
		static void swapNodes(SlabList& list, SlabNode* node, SlabNode* with)
		{
			if (!node || !with)
				return;
			struct SlabNode* aPrev = node->prev;
			struct SlabNode* aNext = node->next;
			struct SlabNode* bPrev = with->prev;
			struct SlabNode* bNext = with->next;
			if (aPrev == with)
			{
				// Assuming the nodes are valid, bNext == node
				node->prev = bPrev;
				node->next = with;
				with->prev = node;
				with->next = aNext;
				if (bPrev) bPrev->next = node;
				if (aNext) aNext->prev = with;
			}
			else if (aNext == with)
			{
				// Assuming the nodes are valid, bPrev == node
				node->prev = with;
				node->next = bNext;
				with->prev = aPrev;
				with->next = node;
				if (bNext) bNext->prev = node;
				if (aPrev) aPrev->next = with;
			}
			else
			{
				node->prev = bPrev;
				node->next = bNext;
				with->prev = aPrev;
				with->next = aNext;
				if (aPrev) aPrev->next = with;
				if (aNext) aPrev->prev = with;
				if (bPrev) bPrev->next = node;
				if (bNext) bNext->prev = node;
			}
			if (list.head == with)
				list.head = node;
			else if (list.head == node)
				list.head = with;
			if (list.tail == with)
				list.tail = node;
			else if (list.tail == node)
				list.tail = with;
		}
		void SlabAllocator::OptimizeAllocator()
		{
			m_lock.Lock();
			ImplOptimizeAllocator();
			m_lock.Unlock();
		}

		SlabAllocator::~SlabAllocator()
		{
			if (!m_base)
				return; // Uninitialized object.
			m_lock.Lock();
			vmm::RawFree(m_base, m_regionSize);
			m_base = nullptr;
			m_regionSize = 0;
			m_allocationSize = 0;
			m_stride = 0;
			m_lock.Unlock();
		}

		// -1: heap corruption.
		//  0: success.
		//  1: error
		static int SortList(SlabList& list, bool ascendingOrder = true)
		{
			bool swapped = false;

			SlabNode* currentNode = list.head, * stepNode = nullptr;
			do
			{
				swapped = false;
				currentNode = list.head;
				if (!currentNode)
					break;

				while (currentNode->next != stepNode)
				{
					if (!currentNode)
						break;
					if (currentNode == currentNode->next)
						return -1;
					bool swap = ascendingOrder ? currentNode > currentNode->next : currentNode < currentNode->next;
					if (swap)
					{
						swapNodes(list, currentNode, currentNode->next);
						swapped = true;
					}
					currentNode = currentNode->next;
				}
				stepNode = currentNode;
			} while (swapped);
			return 0;
		}
		static void CombineContinuousNodes(SlabList& list, size_t stride, size_t allocationSize)
		{
			if (!list.head)
				return;
			auto currentNode = list.head->next;
			while (currentNode)
			{
				auto previousNode = currentNode->prev;
				OBOS_ASSERTP(previousNode != 0, "");
				auto nextNode = currentNode->next;
				// Two blocks that are continuous but in separate nodes.
				if (((uintptr_t)previousNode->data + previousNode->size) == (uintptr_t)currentNode)
				{
					// Combine them.
					previousNode->size += stride - allocationSize + currentNode->size /* Don't add the size, add the entire node + the size, as that's what's being reclaimed.*/;
					list.Remove(currentNode);
				}

				currentNode = nextNode;
			}
		}
		void SlabAllocator::ImplOptimizeAllocator()
		{
			// Sort the list.
			int res = SortList(m_freeNodes, true);
			if (g_kAllocator == this)
				OBOS_ASSERTP(res != -1, "Heap corruption detected.\n");
			// Combine continuous nodes.
			CombineContinuousNodes(m_freeNodes, m_stride, m_allocationSize);
		}

		void SlabList::Append(SlabNode* node)
		{
			if (tail)
				tail->next = node;
			if(!head)
				head = node;
			node->prev = tail;
			tail = node;
			nNodes++;
		}
		void SlabList::Remove(SlabNode* node)
		{
			if (!tail || !head)
				return;
			if (node->prev)
				node->prev->next = node->next;
			if (node->next)
				node->next->prev = node->prev;
			if (tail == node)
				tail = node->prev;
			if (head == node)
				head = node->next;
			node->next = nullptr;
			node->prev = nullptr;
			nNodes--;
		}
	}
}