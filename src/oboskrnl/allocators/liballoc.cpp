/*
	oboskrnl/allocators/liballoc.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <new>

#include <allocators/basic_allocator.h>

#include <locks/spinlock.h>

#include <scheduler/thread.h>
#include <scheduler/scheduler.h>
#include <scheduler/cpu_local.h>

#include <memmanip.h>

#include <vmm/init.h>
#include <vmm/map.h>
#include <vmm/page_node.h>
#include <vmm/page_descriptor.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

#define GET_FUNC_ADDR(addr) reinterpret_cast<uintptr_t>(addr)

#define MIN_PAGES_ALLOCATED 8
#define PTR_ALIGNMENT   0x10
#define ROUND_PTR_UP(ptr) (((ptr) + PTR_ALIGNMENT) & ~(PTR_ALIGNMENT - 1))
#define ROUND_PTR_DOWN(ptr) ((ptr) & ~(PTR_ALIGNMENT - 1))

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

#define makeSafeLock(vName) safe_lock vName{ &m_lock }; vName.Lock();

namespace obos
{
	namespace allocators
	{
		pageBlock* BasicAllocator::allocateNewPageBlock(size_t nPages)
		{
			nPages += (MIN_PAGES_ALLOCATED - (nPages % MIN_PAGES_ALLOCATED));
			pageBlock* blk = nullptr;
			bool isFirstBlock = false;
			if (!m_pageBlockHead && this == allocators::g_kAllocator)
			{
				if (nPages < 16)
					nPages = 16; // Guarantee that there is enough space for any initial page nodes/descriptors.
				isFirstBlock = true;
				blk = (pageBlock*)vmm::RawAllocate(
					(void*)vmm::FindBase(&vmm::g_kernelContext,
						OBOS_KERNEL_ADDRESS_SPACE_USABLE_BASE, 
						OBOS_KERNEL_ADDRESS_SPACE_LIMIT,
						nPages * OBOS_PAGE_SIZE
					),
					nPages * OBOS_PAGE_SIZE,
					vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT,
					0
				);
				memzero(blk, nPages * OBOS_PAGE_SIZE);
			}
			else
			{
				m_lock.Unlock();
				blk = (pageBlock*)vmm::Allocate(
					&vmm::g_kernelContext,
					nullptr,
					nPages * OBOS_PAGE_SIZE, 
					vmm::FLAGS_GUARD_PAGE_LEFT|vmm::FLAGS_GUARD_PAGE_RIGHT,
					0
				);
				m_lock.Lock();
			}
			if(!blk)
				obos::logger::panic(nullptr, "Could not allocate a pageBlock.\n");
			new (blk) pageBlock{};
			blk->magic = PAGEBLOCK_MAGIC;
			blk->nPagesAllocated = nPages;
			blk->nBytesUsed += sizeof(*blk);
			memBlock* firstBlock = (memBlock*)(blk + 1);
			firstBlock->magic = MEMBLOCK_MAGIC;
			firstBlock->allocAddr = firstBlock + 1;
			firstBlock->pageBlock = blk;
			firstBlock->size = blk->nPagesAllocated*OBOS_PAGE_SIZE-((uintptr_t)firstBlock->allocAddr-(uintptr_t)blk);
			blk->freeList.head = blk->freeList.tail = firstBlock;
			blk->freeList.nBlocks++;
			m_totalPagesAllocated += nPages;
			if (m_pageBlockTail)
				m_pageBlockTail->next = blk;
			if(!m_pageBlockHead)
				m_pageBlockHead = blk;
			blk->prev = m_pageBlockTail;
			m_pageBlockTail = blk;
			m_nPageBlocks++;
			if (isFirstBlock)
			{
				m_lock.Unlock();
				void *res = vmm::Allocate(
					&vmm::g_kernelContext, 
					blk, 
					nPages * OBOS_PAGE_SIZE, 
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
		void BasicAllocator::freePageBlock(pageBlock* block)
		{
			if (block->next)
				block->next->prev = block->prev;
			if (block->prev)
				block->prev->next = block->next;
			if (m_pageBlockTail == block)
				m_pageBlockTail = block->prev;
			m_nPageBlocks--;
			m_totalPagesAllocated -= block->nPagesAllocated;
			vmm::Free(&vmm::g_kernelContext, block, block->nPagesAllocated * OBOS_PAGE_SIZE);
		}
		void* BasicAllocator::Allocate(size_t amount)
		{
			if (!amount)
				return nullptr;
	
			makeSafeLock(lock);
	
			amount = ROUND_PTR_UP(amount);
	
			pageBlock* currentPageBlock = nullptr;
	
			// Choose a pageBlock that fits a block with "amount"
	
			size_t amountNeeded = amount + sizeof(memBlock);
	
			if (m_nPageBlocks == 0)
			{
				size_t nPages = (amount / OBOS_PAGE_SIZE) + ((amount % OBOS_PAGE_SIZE) != 0);
				if ((nPages*OBOS_PAGE_SIZE-sizeof(memBlock)-sizeof(pageBlock)) < amount)
					nPages++;
				currentPageBlock = allocateNewPageBlock(nPages);
				if (!currentPageBlock)
					return nullptr;
				goto foundPageBlock;
			}
	
			{
				pageBlock* current = m_pageBlockHead;
	
				// We need to look for pageBlock structures.
				while (current)
				{
					//if (((current->nPagesAllocated * OBOS_PAGE_SIZE) - current->nuint8_tsUsed) >= amountNeeded)
					/*if ((GET_FUNC_ADDR(current->lastBlock->allocAddr) + current->lastBlock->size + sizeof(memBlock) + amountNeeded) <
						(GET_FUNC_ADDR(current) + current->nPagesAllocated * OBOS_PAGE_SIZE))*/
					if (!OBOS_IS_VIRT_ADDR_CANONICAL(current))
						break;
					if (!OBOS_IS_VIRT_ADDR_CANONICAL(current->lastBlock))
						break;
					if (!current->firstBlock || !current->lastBlock)
					{
						// Simply check that.
						if ((current->nPagesAllocated * OBOS_PAGE_SIZE) > amountNeeded)
						{
							currentPageBlock = current;
							break;
						}
						goto end;
					}
					// if ((GET_FUNC_ADDR(current->highestBlock->allocAddr) + current->highestBlock->size + amountNeeded) <=
						// (GET_FUNC_ADDR(current) + current->nPagesAllocated * OBOS_PAGE_SIZE)
						// )
					if (current->freeList.nBlocks)
					{
						currentPageBlock = current;
						break;
					}
	
					end:
					current = current->next;
				};
			}
	
		foundPageBlock:
	
			// We couldn't find a page block big enough :(
			if (!currentPageBlock)
			{
				size_t nPages = (amount / OBOS_PAGE_SIZE) + ((amount % OBOS_PAGE_SIZE) != 0);
				if ((nPages*OBOS_PAGE_SIZE-sizeof(memBlock)-sizeof(pageBlock)) < amount)
					nPages++;
				currentPageBlock = allocateNewPageBlock(nPages);
			}
			if (!currentPageBlock)
				return nullptr;
			
			memBlock* block = nullptr;
			for (auto cur = currentPageBlock->freeList.head; cur; )
			{
				if (cur->size == amount || cur->size >= amountNeeded)
				{
					block = cur;
					break;
				}
				
				cur = cur->next;
			}
			if (!block)
			{
				currentPageBlock = nullptr;
				goto foundPageBlock;
			}
			bool blockRequiresSetup = true;
			if (block->size == amount)
			{
				// Repurpose this block to an allocated block.
				if (currentPageBlock->freeList.head == block)
					currentPageBlock->freeList.head = block->next;
				if (currentPageBlock->freeList.tail == block)
					currentPageBlock->freeList.tail = block->prev;
				if (block->next)
					block->next->prev = block->prev;
				if (block->prev)
					block->prev->next = block->next;
				currentPageBlock->freeList.nBlocks--;
				block->magic = MEMBLOCK_MAGIC;
				blockRequiresSetup = false;
			}
			else if (block->size >= amountNeeded)
			{
				block->size -= amountNeeded;
				if (!block->size)
				{
					if (currentPageBlock->freeList.head == block)
						currentPageBlock->freeList.head = block->next;
					if (currentPageBlock->freeList.tail == block)
						currentPageBlock->freeList.tail = block->prev;
					if (block->next)
						block->next->prev = block->prev;
					if (block->prev)
						block->prev->next = block->next;
					currentPageBlock->freeList.nBlocks--;
				}
				void* ptr = (void*)((uintptr_t)block->allocAddr + block->size);
				block = (memBlock*)ptr;
			}
			else
			{
				currentPageBlock = nullptr;
				goto foundPageBlock;
			}
		foundMemBlock:
			block->next = nullptr;
			block->prev = nullptr;
			if (blockRequiresSetup)
			{
				block->magic = MEMBLOCK_MAGIC;
				block->allocAddr = (void*)(block + 1);
				block->size = amount;
				block->pageBlock = currentPageBlock;
			}
			
			if (currentPageBlock->lastBlock)
				currentPageBlock->lastBlock->next = block;
			if(!currentPageBlock->firstBlock)
				currentPageBlock->firstBlock = block;
			block->prev = currentPageBlock->lastBlock;
			currentPageBlock->lastBlock = block;
			currentPageBlock->nMemBlocks++;
	
			currentPageBlock->nBytesUsed += amountNeeded;
#ifdef OBOS_DEBUG
			block->whoAllocated = (void*)__builtin_extract_return_addr(__builtin_return_address(0));
#endif
			return block->allocAddr;
		}
		void* BasicAllocator::ReAllocate(void* ptr, size_t newSize)
		{
			if (!newSize)
				return nullptr;
	
			newSize = ROUND_PTR_UP(newSize);
	
			if (!ptr)
				return ZeroAllocate(newSize);
	
			size_t oldSize = 0;
	
			memBlock* block = (memBlock*)ptr;
			block--;
			if (block->magic != MEMBLOCK_MAGIC)
				return nullptr;
			oldSize = block->size;
			if (oldSize == newSize)
				return ptr; // The block can be kept in the same state.
			if (newSize < oldSize)
			{
				// If the new size is less than the old size.
				// Truncate the block to the right size.
				block->size = newSize;
				memzero((uint8_t*)ptr + oldSize, oldSize - newSize);
				return ptr;
			}
			// if (block->pageBlock->highestBlock == block && 
				// ((GET_FUNC_ADDR(current) + current->nPagesAllocated * OBOS_PAGE_SIZE) - current->nBytesUsed) >= amountNeeded)
			// {
				// // If we're the highest block in the page block, and there's still space in the page block, then expand the block size.
				// block->size = newSize;
				// memzero((uint8_t*)ptr + oldSize, newSize - oldSize);
				// return ptr;
			// }
			// If the next block crosses page block boundaries, skip checking for that.
			if (((uint8_t*)(block + 1) + oldSize) >= ((uint8_t*)block->pageBlock + block->pageBlock->nPagesAllocated * OBOS_PAGE_SIZE))
				goto newBlock;
			if (((memBlock*)((uint8_t*)(block + 1) + oldSize))->magic != MEMBLOCK_MAGIC)
			{
				// This is rather a corrupted block or there is free space after the block.
				void* blkAfter = ((uint8_t*)(block + 1) + oldSize);
				size_t increment = PTR_ALIGNMENT;
				void* endBlock = nullptr;
				for (endBlock = blkAfter; ((memBlock*)endBlock)->magic != MEMBLOCK_MAGIC &&
					((uint8_t*)endBlock) >= ((uint8_t*)block->pageBlock + block->pageBlock->nPagesAllocated * OBOS_PAGE_SIZE);
					endBlock = (uint8_t*)endBlock + increment);
				size_t nFreeSpace = (uint8_t*)endBlock - (uint8_t*)blkAfter;
				if ((oldSize + nFreeSpace) >= newSize)
				{
					// If we have enough space after the block.
					block->size = newSize;
					memzero((uint8_t*)ptr + oldSize, newSize - oldSize);
					return ptr;
				}
				// Otherwise we'll need a new block.
			}
			newBlock:
			// We need a new block.
			void* newBlock = ZeroAllocate(newSize);
			memcpy(newBlock, ptr, oldSize);
			Free(ptr, 0);
			return newBlock;
		}
		void BasicAllocator::Free(void* ptr, size_t)
		{
			if (!ptr)
				return;
	
			makeSafeLock(lock);
	
			memBlock* block = (memBlock*)ptr;
			block--;
			if (block->magic != MEMBLOCK_MAGIC)
				return;
	
			pageBlock* currentPageBlock = (pageBlock*)block->pageBlock;
	
			const size_t totalSize = sizeof(memBlock) + block->size;
	
			currentPageBlock->nBytesUsed -= totalSize;
	
			if (--currentPageBlock->nMemBlocks)
			{
				if (block->next)
					block->next->prev = block->prev;
				if (block->prev)
					block->prev->next = block->next;
				if (currentPageBlock->lastBlock == block)
					currentPageBlock->lastBlock = block->prev;
				if (currentPageBlock->firstBlock == block)
					currentPageBlock->firstBlock = block->next;
				
				block->next = nullptr;
				block->prev = nullptr;
				
				block->magic = MEMBLOCK_DEAD;
				if (currentPageBlock->freeList.tail)
					currentPageBlock->freeList.tail->next = block;
				if(!currentPageBlock->freeList.head)
					currentPageBlock->freeList.head = block;
				block->prev = currentPageBlock->freeList.tail;
				currentPageBlock->freeList.tail = block;
				currentPageBlock->freeList.nBlocks++;
	
				memzero(block->allocAddr, block->size);
			}
			else
				freePageBlock(currentPageBlock);
		}
		size_t BasicAllocator::QueryObjectSize(const void* ptr)
		{
			memBlock* block = (memBlock*)ptr;
			block--;
			if (block->magic != MEMBLOCK_MAGIC)
				return SIZE_MAX;
			return block->size;
		}
		BasicAllocator::~BasicAllocator()
		{
			for (pageBlock* pb = m_pageBlockHead; pb; )
			{
				auto next = pb->next;
				freePageBlock(pb);
				pb = next;
			}
		}
	}
}