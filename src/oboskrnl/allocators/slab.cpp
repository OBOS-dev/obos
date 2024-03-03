/*
	oboskrnl/allocators/slab.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <todo.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/slab.h>
#include <allocators/slab_structs.h>

#include <vmm/map.h>
#include <vmm/page_descriptor.h>
#include <vmm/init.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

namespace obos
{
	namespace allocators
	{
#define ROUND_UP(n, to) ((to) != 0 ? ((n) / (to) + 1) * (to) : (n))
#define ROUND_UP_COND(n, to) ((to) != 0 ? ((n) / (to) + (((n) % (to)) != 0)) * (to) : (n))
		SlabRegionNode* AllocateRegionNode(void* allocBase, size_t regionSize, size_t stride, size_t allocSize, size_t padding, size_t nodeCount, uintptr_t mapFlags = 0)
		{
			regionSize += sizeof(SlabRegionNode);
			void* base = vmm::Allocate(&vmm::g_kernelContext, allocBase, regionSize, mapFlags, 0);
			if (!base)
				return nullptr;
			SlabRegionNode* ret = (SlabRegionNode*)base;
			memzero(base, regionSize);
			ret->base = ret;
			ret->regionSize = regionSize;
			ret->magic = SLAB_REGION_NODE_MAGIC;
			SlabNode* firstNode = (SlabNode*)ROUND_UP_COND((uintptr_t)(ret + 1), stride);
			new (firstNode) SlabNode{};
			firstNode->magic = SLAB_NODE_MAGIC;
			firstNode->size = allocSize * nodeCount;
			firstNode->data = (char*)ROUND_UP(((uintptr_t)(firstNode + 1) - sizeof(uintptr_t)), padding);
			ret->freeNodes.Append(firstNode);
			return ret;
		}
		static bool CanAllocatePages(void* base, size_t size);
		static void* FindUsableAddress(void* _base, size_t size)
		{
			uintptr_t base = (uintptr_t)_base;
			for (; OBOS_IS_VIRT_ADDR_CANONICAL(base) && base < OBOS_ADDRESS_SPACE_LIMIT; base += OBOS_PAGE_SIZE)
			{
				if (CanAllocatePages((void*)base, size))
					break;
			}
			return (void*)base;
		}
		bool SlabAllocator::Initialize(void* allocBase, size_t allocSize, bool findAddress, size_t initialNodeCount, size_t padding, uintptr_t mapFlags)
		{
			if (!allocSize)
				return false;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(allocBase))
				return false;
			if ((uintptr_t)allocBase < OBOS_KERNEL_ADDRESS_SPACE_BASE)
				logger::warning("Allocation base 0x%p for slab allocator 0x%p, is less than the kernel address space base, 0x%016lx.\n", allocBase, this, OBOS_KERNEL_ADDRESS_SPACE_BASE);
			if (!padding)
				padding = 1;
			if (!initialNodeCount)
				initialNodeCount = OBOS_INITIAL_SLAB_COUNT;
			allocSize = ROUND_UP_COND(allocSize, padding);
			size_t sizeNeeded = ROUND_UP_COND(allocSize + sizeof(SlabNode), padding);
			m_stride = sizeNeeded;
			m_allocationSize = allocSize;
			m_padding = padding;
			if (m_allocBase || findAddress)
			{
				size_t regionSize = m_stride * initialNodeCount;
				regionSize = ROUND_UP_COND(regionSize, padding);
				m_allocBase = FindUsableAddress(allocBase, regionSize);
				SlabRegionNode* node = AllocateRegionNode(findAddress ? nullptr : m_allocBase, regionSize, m_stride, m_allocationSize, padding, initialNodeCount, mapFlags);
				if (!node)
					return false;
				m_regionNodes.Append(node);
			}
			return true;
		}

		bool SlabAllocator::AddRegion(void* base, size_t regionSize)
		{
			if (regionSize < sizeof(SlabRegionNode))
				return false;
			if ((regionSize - sizeof(SlabRegionNode)) < m_stride)
				return false;
			SlabRegionNode* node = (SlabRegionNode*)base;
			new (node) SlabRegionNode{};
			node->magic = SLAB_REGION_NODE_MAGIC;
			node->base = base;
			node->regionSize = regionSize;
			SlabNode* firstNode = (SlabNode*)(node + 1);
			new (firstNode) SlabNode{};
			firstNode->magic = SLAB_NODE_MAGIC;
			firstNode->size = regionSize - sizeof(SlabNode) - sizeof(SlabRegionNode);
			firstNode->data = (char*)ROUND_UP(((uintptr_t)(firstNode + 1) - sizeof(uintptr_t)), m_padding);
			node->freeNodes.Append(firstNode);
			m_regionNodes.Append(node);
			return true;
		}

		static void* AllocateNode(SlabList& freeList, SlabList& allocatedList, SlabNode* node, size_t sz, size_t padding)
		{
			size_t requiredSize = ROUND_UP_COND(sz + sizeof(SlabNode), padding);
			if (node->size == sz)
				requiredSize = sz;
			if (node->size < requiredSize)
				return nullptr;
			node->size -= requiredSize;
			if (!node->size)
				freeList.Remove(node);
			SlabNode* newNode = (SlabNode*)(node->data + node->size);
			if (requiredSize == sz)
				newNode = node;
			memzero(newNode, sizeof(*newNode));
			newNode->magic = SLAB_NODE_MAGIC;
			newNode->size = sz;
			newNode->data = (char*)ROUND_UP(((uintptr_t)(newNode + 1) - sizeof(uintptr_t)), padding);
			allocatedList.Append(newNode);
			return newNode->data;
		}
		void* SlabAllocator::AllocateFromRegion(SlabRegionNode* region, size_t size)
		{
			void* ret = nullptr;
			region->lock.Lock();
			if (!region->freeNodes.nNodes)
			{
				region->lock.Unlock();
				return nullptr;
			}
			ImplOptimizeList(region->freeNodes);
			for (SlabNode* node = region->freeNodes.tail; node;)
			{
				if (this == g_kAllocator)
					OBOS_ASSERTP(node->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , node, node->size, node->data);
				else
					OBOS_ASSERT(node->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , node, node->size, node->data);

				if ((ret = AllocateNode(region->freeNodes, region->allocatedNodes, node, size, m_padding)))
					break;
				node = node->prev;
			}
			region->lock.Unlock();
			return ret;
		}
		static bool CanAllocatePages(void* base, size_t size)
		{
			size_t nPages = ROUND_UP_COND(size, OBOS_PAGE_SIZE) / OBOS_PAGE_SIZE;
			uintptr_t _base = (uintptr_t)base;
			vmm::page_descriptor pd{};
			for (uintptr_t addr = (uintptr_t)base; addr < (_base + (nPages * OBOS_PAGE_SIZE)); addr += OBOS_PAGE_SIZE)
			{
				arch::get_page_descriptor((vmm::Context*)nullptr, (void*)addr, pd);
				if (pd.present)
					return false;
			}
			return true;
		}
		void* SlabAllocator::Allocate(size_t size)
		{
			void* ret = nullptr;
			size *= m_allocationSize;
			// m_allocationSize is rounded to the padding.
			//size = ROUND_UP_COND(size, m_padding);
			for (SlabRegionNode* cregion = m_regionNodes.head; cregion;)
			{
				if ((ret = AllocateFromRegion(cregion, size)))
					break;

				cregion = cregion->next;
			}
			if (!ret)
			{
				// Allocate a new region.
				size_t regionSize = ROUND_UP_COND(size + m_allocationSize * OBOS_INITIAL_SLAB_COUNT, m_allocationSize);
				SlabRegionNode* newRegion = AllocateRegionNode(nullptr, regionSize, m_stride, m_allocationSize, m_padding, regionSize / m_allocationSize);
				m_regionNodes.Append(newRegion);
				return AllocateFromRegion(newRegion, size);
			}
			return ret;
		}

		SlabNode* SlabAllocator::LookForNode(SlabList &list, void* addr)
		{
			for (SlabNode* node = list.head; node;)
			{
				if (this == g_kAllocator)
					OBOS_ASSERTP(node->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.",, node, node->size, node->data);
				else
					OBOS_ASSERT(node->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , node, node->size, node->data);

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
			SlabNode* node = nullptr;
			SlabRegionNode* region = nullptr;
			for (SlabRegionNode* cregion = m_regionNodes.head; cregion;)
			{
				if (!(base >= cregion->base && base < ((char*)cregion->base + cregion->regionSize)))
					goto bottom;
				node = LookForNode(cregion->allocatedNodes, base);
				if (node)
				{
					region = cregion;
					break;
				}

			bottom:
				cregion = cregion->next;
			}
			if (!node || !region)
				return;
			region->lock.Lock();
			memzero(node->data, node->size);
			region->allocatedNodes.Remove(node);
			node->next = nullptr;
			node->prev = nullptr;
			region->freeNodes.Append(node);
			region->lock.Unlock();
		}

		size_t SlabAllocator::QueryObjectSize(void* base)
		{
			SlabNode* node = nullptr;
			for (SlabRegionNode* region = m_regionNodes.head; region;)
			{
				if (!(base >= region->base && base < ((char*)region->base + region->regionSize)))
					goto bottom;
				node = LookForNode(region->allocatedNodes, base);
				if (node)
					break;

			bottom:
				region = region->next;
			}
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
			size_t nFreeRegionNodes = 0;
			for (SlabRegionNode* node = m_regionNodes.head; node;)
			{
				nFreeRegionNodes += (node->allocatedNodes.nNodes == 0);

				node = node->next;
			}
			for (SlabRegionNode* node = m_regionNodes.head; node;)
			{
				node->lock.Lock();
				ImplOptimizeList(node->freeNodes);
				if (nFreeRegionNodes >= m_maxEmptyRegionNodesAllowed && node->allocatedNodes.nNodes == 0)
				{
					node->lock.Lock();
					node->base = nullptr;
					node->regionSize = 0;
					m_regionNodes.Remove(node);
					memzero(node->base, node->regionSize);
					vmm::Free(&vmm::g_kernelContext, node->base, node->regionSize);
					//node->lock.Unlock();
					nFreeRegionNodes--;
				}
				node->lock.Unlock();

				node = node->next;
			}
		}

		SlabAllocator::~SlabAllocator()
		{
			for (SlabRegionNode* node = m_regionNodes.head; node;)
			{
				node->lock.Lock();
				node->base = nullptr;
				node->regionSize = 0;
				node->lock.Unlock();

				SlabRegionNode* temp = node;
				node = node->next;
				memzero(temp, sizeof(*temp)); // Nuke the node.
				vmm::Free(&vmm::g_kernelContext, node->base, node->regionSize);
			}
			// Nuke the region node list.
			memzero(&m_regionNodes, sizeof(m_regionNodes));
			
			m_allocationSize = 0;
			m_stride = 0;
			m_padding = 0;
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
		void SlabAllocator::CombineContinuousNodes(SlabList& list)
		{
			if (!list.head)
				return;
			auto currentNode = list.head->next;
			while (currentNode)
			{
				auto previousNode = currentNode->prev;
				if (this == g_kAllocator)
				{
					OBOS_ASSERTP(currentNode->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%lu, data=0x%p.", , currentNode, currentNode->size, currentNode->data);
					OBOS_ASSERTP(previousNode != 0, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , currentNode, currentNode->size, currentNode->data);
				}
				else
				{
					OBOS_ASSERT(currentNode->magic == SLAB_NODE_MAGIC, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , currentNode, currentNode->size, currentNode->data);
					OBOS_ASSERT(previousNode != 0, "Heap corruption detected for node 0x%p. size=%ld, data=0x%p.", , currentNode, currentNode->size, currentNode->data);
				}
				auto nextNode = currentNode->next;
				// Two blocks that are continuous but in separate nodes.
				if (((uintptr_t)previousNode->data + previousNode->size) == (uintptr_t)currentNode)
				{
					// Combine them.
					previousNode->size += m_stride - m_allocationSize + currentNode->size /* Don't add the size, add the entire node + the size, as that's what's being reclaimed.*/;
					list.Remove(currentNode);
				}

				currentNode = nextNode;
			}
		}
		void SlabAllocator::ImplOptimizeList(SlabList& list)
		{
			// Sort the list.
			int res = SortList(list, true);
			if (g_kAllocator == this)
				OBOS_ASSERTP(res != -1, "Heap corruption detected.\n");
			// Combine continuous nodes.
			CombineContinuousNodes(list);
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
		void SlabRegionList::Append(SlabRegionNode* node)
		{
			if (tail)
				tail->next = node;
			if (!head)
				head = node;
			node->prev = tail;
			tail = node;
			nNodes++;
		}
		void SlabRegionList::Remove(SlabRegionNode* node)
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