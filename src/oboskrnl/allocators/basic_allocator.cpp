/*
	oboskrnl/allocators/basic_allocator.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <new>

#include <allocators/basic_allocator.h>

#include <locks/spinlock.h>

#include <vmm/map.h>
#include <vmm/prot.h>
#include <vmm/init.h>
#include <vmm/page_node.h>

#include <arch/vmm_map.h>
#include <arch/vmm_defines.h>

struct safe_lock
{
	safe_lock() = delete;
	safe_lock(obos::locks::SpinLock* lock)
	{
		m_lock = lock;
	}
	bool Lock()
	{
		if (m_lock)
			return m_lock->Lock();
		return false;
	}
	bool IsLocked()
	{
		if (m_lock)
			return m_lock->Locked();
		return false;
	}
	void Unlock()
	{
		if (m_lock)
			m_lock->Unlock();
	}
	~safe_lock()
	{
		Unlock();
	}
private:
	obos::locks::SpinLock* m_lock = nullptr;
};

#define makeSafeLock(vName) safe_lock vName{ &m_lock }; vName.Lock()

namespace obos
{
	namespace allocators
	{
		constexpr uintptr_t round_up(uintptr_t x, size_t to)
		{
			if (x % to)
				return x + (to-(x%to));
			return x;
		}
		void *BasicAllocator::Allocate(size_t size)
		{
			size = round_up(size, 0x10);
			// First, find a region.
			BasicAllocator::region* from = nullptr;
			BasicAllocator::region* start = m_regionHead;
			tryAgain:
			for (auto r = start; r; )
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
				from = allocateNewRegion(size);
			if (!from)
				return nullptr; // uh-oh.
			// Then, use that region's first free node with a size big enough to handle this allocation.
			BasicAllocator::node *freeNode = nullptr;
			for (auto n = from->free.head; n; )
			{
				if (n->magic != MEMBLOCK_MAGIC || n->size > from->size || n->_containingRegion != from)
					logger::panic(nullptr, "Memory corruption detected for block %p. Dumping node contents.\nn->magic: 0x%08x, n->size: %ld, n->_containingRegion: 0x%p, n->next: 0x%p, n->prev: 0x%p, allocAddr: 0x%p\n", n, n->magic, n->size, n->_containingRegion, n->next, n->prev, n->getAllocAddr());
				if (n->size == size)
				{
					makeSafeLock(lock);
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
						node* res = nullptr;
						for (auto n = from->free.head; n; )
						{
							if (!res || n->size > res->size)
								res = n;
							
							n = n->next;
						}
						from->biggestFreeNode = res;
					}
					break;
				}
				if (n->size >= (size + sizeof(node)))
				{
					// This'll work as long as we suballocate within the node.
					makeSafeLock(lock);
					n->size -= size + sizeof(node);
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
							node* res = nullptr;
							for (auto n = from->free.head; n; )
							{
								if (!res || n->size > res->size)
									res = n;
								
								n = n->next;
							}
							from->biggestFreeNode = res;
						}
					}
					node* newNode = (node*)((char*)n->getAllocAddr() + n->size);
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
			makeSafeLock(lock);
			freeNode->next = freeNode->prev = nullptr;
			freeNode->magic = MEMBLOCK_MAGIC;
			freeNode->_containingRegion = from;
			freeNode->size = size;
			if (from->allocated.tail)
				from->allocated.tail->next = freeNode;
			if(!from->allocated.head)
				from->allocated.head = freeNode;
			freeNode->prev = from->allocated.tail;
			from->allocated.tail = freeNode;
			from->allocated.nNodes++;
			from->nFreeBytes -= size;
			return freeNode->getAllocAddr();
		}
		void* BasicAllocator::ReAllocate(void* base, size_t newSize)
		{
			if (!base)
				return Allocate(newSize);
			newSize = round_up(newSize, 0x10);
			size_t objSize = QueryObjectSize(base);
			if (objSize == SIZE_MAX)
				return nullptr;
			if (objSize == newSize)
				return base;
			if (newSize < objSize)
			{
				BasicAllocator::node* n = ((node*)base - 1);
				memzero((char*)base + n->size, newSize-objSize);
				n->size = newSize;
				return base;
			}
			// The block is bigger.
			void* newBlock = Allocate(newSize);
			memcpy(newBlock, base, objSize);
			Free(base, objSize);
			return newBlock;
		}
		void BasicAllocator::Free(void* base, size_t)
		{
			BasicAllocator::node* n = ((node*)base - 1);
			if (n->magic != MEMBLOCK_MAGIC)
				return;
			auto r = (region*)n->_containingRegion;
			if (!r)
				return;
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
				freeRegion(r);
				return;
			}
			n->next = n->prev = nullptr;
			if (r->free.tail)
				r->free.tail->next = n;
			if(!r->free.head)
				r->free.head = n;
			n->prev = r->free.tail;
			r->free.tail = n;
			r->free.nNodes++;
			r->nFreeBytes += n->size;
			if (!r->biggestFreeNode)
				return;
			if (n->size > r->biggestFreeNode->size)
				r->biggestFreeNode = n;
		}
		size_t BasicAllocator::QueryObjectSize(const void* base)
		{
			if (!base)
				return SIZE_MAX;
			auto n = ((const BasicAllocator::node*)base - 1);
			if (n->magic != MEMBLOCK_MAGIC)
				return SIZE_MAX;
			if (!n->_containingRegion)
				return SIZE_MAX;
			return n->size;
		}
		BasicAllocator::region* BasicAllocator::allocateNewRegion(size_t size)
		{
			size = round_up(size, OBOS_PAGE_SIZE*4);
			size += sizeof(region) + sizeof(node);
			size_t initialSize = size;
			makeSafeLock(lock);
			BasicAllocator::region* blk = nullptr;
			if (this == &vmm::g_vmmAllocator)
			{
				blk = (region*)vmm::RawAllocate(
					(void*)vmm::FindBase(&vmm::g_kernelContext,
						OBOS_KERNEL_ADDRESS_SPACE_USABLE_BASE, 
						OBOS_KERNEL_ADDRESS_SPACE_LIMIT,
						size
					),
					size,
					vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT,
					0
				);
				memzero(blk, size);
			}
			else
			{
				m_lock.Unlock();
				blk = (region*)vmm::Allocate(
					&vmm::g_kernelContext,
					nullptr,
					size, 
					vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT,
					0
				);
				m_lock.Lock();
			}
			new (blk) region{};
			blk->size = initialSize+sizeof(node);
			BasicAllocator::node *n = (node*)(blk + 1);
			new (n) node{};
			n->size = initialSize;
			n->next = n->prev = nullptr;
			n->_containingRegion = blk;
			blk->biggestFreeNode = n;
			blk->free.tail = blk->free.head = n;
			blk->free.nNodes++;
			blk->nFreeBytes += n->size;
			if (m_regionTail)
				m_regionTail->next = blk;
			if(!m_regionHead)
				m_regionHead = blk;
			blk->prev = m_regionTail;
			m_regionTail = blk;
			m_nRegions++;
			if (this == &vmm::g_vmmAllocator)
			{
				m_lock.Unlock();
				void *res = vmm::Allocate(
					&vmm::g_kernelContext, 
					blk, 
					size, 
					vmm::FLAGS_RESERVE|vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT,
					0
				);
				OBOS_ASSERTP(res, "Could not reserve page block.\n");
				auto pgNode = vmm::g_kernelContext.GetPageNode(res);
				OBOS_ASSERTP(pgNode, "No page node found");
				for (size_t i = 0; i < pgNode->nPageDescriptors; i++)
					arch::get_page_descriptor(&vmm::g_kernelContext, (void*)pgNode->pageDescriptors[i].virt, pgNode->pageDescriptors[i]);
				m_lock.Lock();
			}
			return blk;
		}
		void BasicAllocator::freeRegion(BasicAllocator::region* block)
		{
			if (block->prev)
				block->prev->next = block->next;
			if (block->next)
				block->next->prev = block->prev;
			if (m_regionHead == block)
				m_regionHead = block->next;
			if (m_regionTail == block)
				m_regionTail = block->prev;
			m_nRegions--;
			vmm::Free(&vmm::g_kernelContext, block, block->size+sizeof(*block));
		}
		BasicAllocator::~BasicAllocator()
		{
			for (region* pb = m_regionHead; pb; )
			{
				auto next = pb->next;
				freeRegion(pb);
				pb = next;
			}
		}
	}
}