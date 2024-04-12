/*
	oboskrnl/vmm/map.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <todo.h>
#include <memmanip.h>

#include <arch/vmm_map.h>
#include <arch/vmm_defines.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>
#include <vmm/page_node.h>
#include <vmm/prot.h>
#include <vmm/map.h>
#include <vmm/init.h>

namespace obos
{
	namespace vmm
	{
		// This function will most likely be optimized away, except for logger::panic.
		// However, it will still be checked for errors, like any other function, so we can do this to verify arch-specific functions.
		static void verify_arch_specific()
		{
			logger::panic(nullptr, "%s called!", __func__);
			// Verify all arch-specific functions are defined by the platform.
			// If any of the necessary functions don't exist, or take invalidly typed parameters, the compilation will fail.
			// If you are porting to a different arch, and need a reference implementation, look at the x86-64 implementation, as it is documented.
			[[maybe_unused]] void *r1 = arch::map_page_to((Context*)nullptr, 0,0,0);
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			[[maybe_unused]] void *r2 = arch::map_hugepage_to((Context*)nullptr, 0,0,0);
#endif
			arch::unmap((Context*)nullptr, 0);
			arch::get_page_descriptor((Context*)nullptr, 0, *(page_descriptor*)nullptr);
			[[maybe_unused]] bool r3 = arch::register_page_fault_handler(PageFaultReason::PageFault_AccessViolation, false, nullptr);
			[[maybe_unused]] uintptr_t r4 = arch::AllocatePhysicalPages(0, false);
			arch::FreePhysicalPages(0, 0);
		}

		void* RawAllocate(void* _where, size_t size, allocflag_t flags, prot_t protection)
		{
#if OBOS_HAS_HUGE_PAGE_SUPPORT == 0
			COMPILE_MESSAGE("OBOS_HAS_HUGE_PAGE_SUPPORT being zero hasn't tested, remove this on the first architecture (after fixing any bugs) that doesn't implement huge pages.\n");
			if ((flags & FLAGS_USE_HUGE_PAGES))
				return nullptr;
#endif
			protection |= PROT_NO_DEMAND_PAGE;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			bool allocateHugePages = (flags & FLAGS_USE_HUGE_PAGES);
			size_t pageSize = allocateHugePages ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr bool allocateHugePages = false;
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			uintptr_t where = (uintptr_t)_where;
			where -= (where % pageSize);
			size += (pageSize - (size % pageSize));
			if (!where || !size)
				return nullptr;
			// Loop through the pages, mapping them.
			if ((flags & FLAGS_DISABLE_HUGEPAGE_OPTIMIZATION) && !allocateHugePages)
			{
				for (uintptr_t addr = where; addr < (where + size); addr += pageSize)
					arch::map_page_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(pageSize / OBOS_PAGE_SIZE, allocateHugePages), protection);
			}
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			else
			{
				// We can use this to allocate huge pages, both as an optimization and if it was specifically requested (flags & FLAGS_USE_HUGE_PAGES).

				size_t nHugePages = size / OBOS_HUGE_PAGE_SIZE;
				size_t nPagesInitial = 0;
				if (nHugePages)
					nPagesInitial = (where % OBOS_HUGE_PAGE_SIZE) / OBOS_PAGE_SIZE;
				size_t nPagesLeftOver = (size - (nHugePages * OBOS_HUGE_PAGE_SIZE)) / OBOS_PAGE_SIZE;
				uintptr_t addr = where;
				for (size_t i = 0; i < nPagesInitial; i++, addr += OBOS_PAGE_SIZE)
					arch::map_page_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(1, false), protection);
				for (size_t i = 0; i < nHugePages; i++, addr += OBOS_HUGE_PAGE_SIZE)
					arch::map_hugepage_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE / OBOS_PAGE_SIZE, true), protection);
				for (size_t i = 0; i < nPagesLeftOver; i++, addr += OBOS_PAGE_SIZE)
					arch::map_page_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(1, false), protection);
			}
#endif
			return (void*)where;
		}
		bool RawFree(void* _where, size_t size)
		{
			uintptr_t where = (uintptr_t)_where;
			page_descriptor pd = {};
			arch::get_page_descriptor((Context*)nullptr, (void*)where, pd);
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			size_t pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			if (where % pageSize)
				where -= (where % pageSize);
			if (size % pageSize)
				size += (pageSize - (size % pageSize));
			if (!where || !size)
				return false;
			// Loop through the pages, un-mapping and freeing them.
			for (uintptr_t addr = where; addr < (where + size); addr += pageSize)
			{
				arch::get_page_descriptor((Context*)nullptr, (void*)addr, pd);
				if (!pd.present)
					break;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
				pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#endif
				if (pd.phys && !pd.awaitingDemandPagingFault)
					arch::FreePhysicalPages(pd.phys, pageSize / OBOS_PAGE_SIZE);
				arch::unmap((Context*)nullptr, (void*)addr);
			}
			return true;
		}
		
		static bool _InRange(uintptr_t base, uintptr_t end, uintptr_t val)
		{
			return (val >= base) && (val < end);
		}
#define InRange(base, end, val) (obos::vmm::_InRange((uintptr_t)(base), (uintptr_t)(end), (uintptr_t)(val)))
		bool MapPageDescriptor(Context* ctx, const page_descriptor& pd)
		{
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			size_t pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			uintptr_t virt = pd.virt - (pd.virt % pageSize);
			uintptr_t phys = pd.phys - (pd.phys % pageSize);
			OBOS_ASSERTP(ctx, "ctx is nullptr");
			if (!ctx)
				return false;
			page_node node;
			memzero(&node, sizeof(node));
			node.ctx = ctx;
			node.nPageDescriptors = 1;
			node.pageDescriptors = new page_descriptor{};
			node.pageDescriptors[0].isHugePage = pd.isHugePage;
			node.pageDescriptors[0].protFlags = pd.protFlags;
			node.pageDescriptors[0].present = pd.present;
			node.pageDescriptors[0].phys = phys;
			node.pageDescriptors[0].virt = virt;
			ctx->AppendPageNode(node);
			if (!node.pageDescriptors[0].isHugePage)
				arch::map_page_to(ctx, virt, phys, pd.protFlags);
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			else 
				arch::map_hugepage_to(ctx, virt, phys, pd.protFlags);
#endif
			return true;
		}
		uintptr_t FindBase(Context* ctx, uintptr_t base, uintptr_t limit, size_t size)
		{
			if (base % OBOS_PAGE_SIZE)
				base -= (base % OBOS_PAGE_SIZE);
			if (limit % OBOS_PAGE_SIZE)
				limit += (OBOS_PAGE_SIZE - (limit % OBOS_PAGE_SIZE));
			if (size % OBOS_PAGE_SIZE)
				size += (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
			if (limit < base)
				return 0;
			if ((limit - base) < size)
				return 0;
			uintptr_t lastAddress = base;
			for (const page_node* node = ctx->m_head; node;)
			{
				bool exitedGracefully = true;
				for (size_t j = 0; j < node->nPageDescriptors; j++)
				{
					uintptr_t virt = node->pageDescriptors[j].virt;
					if (!(exitedGracefully = !(virt < base)))
						continue;
					if (virt >= limit)
						return 0;
					if ((virt - lastAddress) >= (size + OBOS_PAGE_SIZE))
						return lastAddress;
					lastAddress = virt;
				}
				if (exitedGracefully)
					lastAddress += node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? (uintptr_t)OBOS_HUGE_PAGE_SIZE : (uintptr_t)OBOS_PAGE_SIZE;
				
				node = node->next;
			}
			const page_node* node = ctx->m_tail;
			if (!node)
				return base;
			auto &pd = node->pageDescriptors[node->nPageDescriptors - 1];
			return pd.virt + (pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
		}
		static bool CanAllocate(Context* ctx, void* base, size_t size)
		{
#if !OBOS_HAS_HUGE_PAGE_SUPPORT
			// Temporarily define OBOS_HUGE_PAGE_SIZE because I'm lazy.
#define OBOS_HUGE_PAGE_SIZE OBOS_PAGE_SIZE
#endif
			for (const page_node* node = ctx->GetHead(); node;)
			{
				if (InRange(node->pageDescriptors[0].virt, 
					node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE), 
					base) &&
					InRange(node->pageDescriptors[0].virt, 
					node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE), 
					(uintptr_t)base + size))
					return false;
				node = node->next;
#if !OBOS_HAS_HUGE_PAGE_SUPPORT
#undef OBOS_HUGE_PAGE_SIZE
#endif
			}
			return true;
		}
		// Referenced in mprot.cpp
		bool IsAllocated(Context* ctx, void* base, size_t size)
		{
#if !OBOS_HAS_HUGE_PAGE_SUPPORT
			// Temporarily define OBOS_HUGE_PAGE_SIZE because I'm lazy.
#define OBOS_HUGE_PAGE_SIZE OBOS_PAGE_SIZE
#endif
			for (const page_node* node = ctx->GetHead(); node;)
			{
				if (InRange(node->pageDescriptors[0].virt, 
					node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE), 
					base) &&
				   InRange(node->pageDescriptors[0].virt, 
					node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE), 
					(uintptr_t)base + size))
					return true;
				node = node->next;
#if !OBOS_HAS_HUGE_PAGE_SUPPORT
#undef OBOS_HUGE_PAGE_SIZE
#endif
			}
			return false;
		}
		static bool IsCommitted(Context* ctx, void* base, size_t size)
		{
			for (const page_node* node = ctx->GetHead(); node;)
			{
				if (!(InRange(node->pageDescriptors[0].virt,
					node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
					base) &&
					InRange(node->pageDescriptors[0].virt,
						node->pageDescriptors[node->nPageDescriptors - 1].virt + (node->pageDescriptors[node->nPageDescriptors - 1].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
						(uintptr_t)base + size - 1)))
				{
					node = node->next;
					continue;
				}
				size_t i = 0;
				for (; i < node->nPageDescriptors; i++)
					if (InRange(node->pageDescriptors[i].virt,
						node->pageDescriptors[i].virt + (node->pageDescriptors[i].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
						base))
						break;
				size_t j = 0;
				for (; j < node->nPageDescriptors; i++)
					if (InRange(node->pageDescriptors[j].virt,
						node->pageDescriptors[j].virt + (node->pageDescriptors[j].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
						(char*)base + size - 1))
						break;
				OBOS_ASSERTP(InRange(node->pageDescriptors[i].virt,
					node->pageDescriptors[i].virt + (node->pageDescriptors[i].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
					base), "Could not find base in page descriptor table of node.\n");
				OBOS_ASSERTP(InRange(node->pageDescriptors[j].virt,
					node->pageDescriptors[j].virt + (node->pageDescriptors[j].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
					base), "Could not find limit in page descriptor table of node.\n");
				for (; i < j; i++)
					if (!node->pageDescriptors[j].present)
						return false;
				return true;
				node = node->next;
			}
			return false;
		}
		static void ImplAllocateSmallPages(Context* ctx, uintptr_t where, size_t size, page_node& node, bool present, prot_t protection, allocflag_t flags, size_t i = 0)
		{
			bool isFirstPage = true;
			for (uintptr_t addr = where; addr < (where + size); addr += OBOS_PAGE_SIZE)
			{
				node.pageDescriptors[i].isHugePage = false;
				node.pageDescriptors[i].present = (isFirstPage && flags & FLAGS_GUARD_PAGE_LEFT) ? false : present;
				if (protection & PROT_NO_DEMAND_PAGE && node.pageDescriptors[i].present)
					node.pageDescriptors[i].phys = arch::AllocatePhysicalPages(1, false);
				else
					node.pageDescriptors[i].phys = 0;
				node.pageDescriptors[i].virt = addr;
				node.pageDescriptors[i].protFlags = protection;
				if (node.pageDescriptors[i].present)
					arch::map_page_to(ctx, addr, node.pageDescriptors[i].phys, protection);
				isFirstPage = false;
			}
		}
		static void ImplAllocateHugePages(Context* ctx, uintptr_t where, size_t size, page_node& node, prot_t protection, allocflag_t flags, bool present, size_t j = 0)
		{
			size_t nHugePages = size / OBOS_HUGE_PAGE_SIZE;
			size_t nPagesInitial = 0;
			if (nHugePages)
				nPagesInitial = (where % OBOS_HUGE_PAGE_SIZE) / OBOS_PAGE_SIZE;
			size_t nPagesLeftOver = (size - (nHugePages * OBOS_HUGE_PAGE_SIZE)) / OBOS_PAGE_SIZE;
			uintptr_t addr = where;
			bool isFirstPage = true;
			for (size_t i = 0; i < nPagesInitial; i++, j++, addr += OBOS_PAGE_SIZE)
			{
				node.pageDescriptors[j].isHugePage = false;
				if (isFirstPage && (flags & FLAGS_GUARD_PAGE_LEFT))
					node.pageDescriptors[j].present = false;
				else
					node.pageDescriptors[j].present = present;
				if (protection & PROT_NO_DEMAND_PAGE && node.pageDescriptors[j].present)
					node.pageDescriptors[j].phys = arch::AllocatePhysicalPages(1, false);
				else
					node.pageDescriptors[j].phys = 0;
				node.pageDescriptors[j].virt = addr;
				if ((i == (nPagesInitial - 1) && !nPagesLeftOver && !nHugePages) && (flags & FLAGS_GUARD_PAGE_RIGHT))
					node.pageDescriptors[j].present = false;
				node.pageDescriptors[j].protFlags = protection;
				if (node.pageDescriptors[j].present)
					arch::map_page_to(ctx, addr, node.pageDescriptors[j].phys, protection);
				isFirstPage = false;
			}
			for (size_t i = 0; i < nHugePages; i++, j++, addr += OBOS_HUGE_PAGE_SIZE)
			{
				node.pageDescriptors[j].isHugePage = true;
				if (isFirstPage && (flags & FLAGS_GUARD_PAGE_LEFT))
					node.pageDescriptors[j].present = false;
				else
					node.pageDescriptors[j].present = present;
				if (protection & PROT_NO_DEMAND_PAGE && node.pageDescriptors[j].present)
					node.pageDescriptors[j].phys = arch::AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE / OBOS_PAGE_SIZE, true);
				else
					node.pageDescriptors[j].phys = 0;
				node.pageDescriptors[j].virt = addr;
				if ((i == (nHugePages - 1) && !nPagesLeftOver) && (flags & FLAGS_GUARD_PAGE_RIGHT))
					node.pageDescriptors[j].present = false;
				node.pageDescriptors[j].protFlags = protection;
				if (node.pageDescriptors[j].present)
					arch::map_hugepage_to(ctx, addr, node.pageDescriptors[j].phys, protection);
				isFirstPage = false;
			}
			for (size_t i = 0; i < nPagesLeftOver; i++, j++, addr += OBOS_PAGE_SIZE)
			{
				node.pageDescriptors[j].isHugePage = false;
				if (isFirstPage && (flags & FLAGS_GUARD_PAGE_LEFT))
					node.pageDescriptors[j].present = false;
				else
					node.pageDescriptors[j].present = present;
				if (protection & PROT_NO_DEMAND_PAGE && node.pageDescriptors[j].present)
					node.pageDescriptors[j].phys = arch::AllocatePhysicalPages(1, false);
				else
					node.pageDescriptors[j].phys = 0;
				node.pageDescriptors[j].virt = addr;
				if ((i == (nPagesLeftOver - 1)) && (flags & FLAGS_GUARD_PAGE_RIGHT))
					node.pageDescriptors[j].present = false;
				node.pageDescriptors[j].protFlags = protection;
				if (node.pageDescriptors[j].present)
					arch::map_page_to(ctx, addr, node.pageDescriptors[j].phys, protection);
				isFirstPage = false;
			}
		}
		// Handles guard pages being allocated, huge pages, requests to disable huge page optimizations, and reserving and committing pages.
		static bool ImplAllocatePages(Context* ctx, void* _base, size_t size, prot_t protection, allocflag_t flags)
		{
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			bool allocateHugePages = (flags & FLAGS_USE_HUGE_PAGES);
			size_t pageSize = allocateHugePages	? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr bool allocateHugePages = false;
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			// Whether the page should be present or not.
			const bool present = !(flags & FLAGS_RESERVE) || (flags & FLAGS_COMMIT);
			uintptr_t where = (uintptr_t)_base;
			if (IsAllocated(ctx, _base, size) && !IsCommitted(ctx, _base, size))
			{
				if (!(flags & FLAGS_COMMIT))
					return false;
				TODO("Test committing reserved pages.");
				// Retrieve the node.
				page_node* node = ctx->GetPageNode(_base);

				size_t i = 0;
				for (; i < node->nPageDescriptors; i++)
					if (InRange(node->pageDescriptors[0].virt,
						node->pageDescriptors[node->nPageDescriptors - 1].virt,
						_base
					))
						break;
				ctx->Lock();
				if ((node->allocFlags & FLAGS_DISABLE_HUGEPAGE_OPTIMIZATION) && !(node->allocFlags & FLAGS_USE_HUGE_PAGES))
					ImplAllocateSmallPages(ctx, where, size, *node, present, protection, flags, i);
				else
					ImplAllocateHugePages(ctx, where, size, *node, protection, flags, present, i);
				ctx->Unlock();
				return true;
			}
			page_node node;
			memzero(&node, sizeof(node));
			node.allocFlags = flags;
			node.ctx = ctx;
			// Loop through the pages, mapping them.
			if ((flags & FLAGS_DISABLE_HUGEPAGE_OPTIMIZATION) && !allocateHugePages)
			{
				node.nPageDescriptors = size / pageSize;
				node.pageDescriptors = new page_descriptor[node.nPageDescriptors];
				
				ImplAllocateSmallPages(ctx, where, size, node, present, protection, flags);
			}
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			else
			{
				// We can use this to allocate huge pages, both as an optimization and if it was specifically requested (flags & FLAGS_USE_HUGE_PAGES).
				size_t nHugePages = size / OBOS_HUGE_PAGE_SIZE;
				size_t nPagesInitial = 0;
				if (nHugePages)
					nPagesInitial = (where % OBOS_HUGE_PAGE_SIZE) / OBOS_PAGE_SIZE;
				size_t nPagesLeftOver = (size - (nHugePages * OBOS_HUGE_PAGE_SIZE)) / OBOS_PAGE_SIZE;

				node.nPageDescriptors = nHugePages + nPagesInitial + nPagesLeftOver;
				node.pageDescriptors = new page_descriptor[node.nPageDescriptors];
				ImplAllocateHugePages(ctx, where, size, node, protection, flags, present);
			}
#endif
			ctx->AppendPageNode(node);
			return true;
		}
		void* Allocate(Context* ctx, void* _base, size_t size, allocflag_t flags, prot_t protection)
		{
			if (!ctx)
				return nullptr;
			uintptr_t base = (uintptr_t)_base;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			bool isHugePage = flags & FLAGS_USE_HUGE_PAGES;
			size_t pageSize = isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			if (base % pageSize)
				base -= (base % pageSize);
			if (size % pageSize)
				size += (pageSize - (size % pageSize));
			if ((flags & FLAGS_GUARD_PAGE_LEFT))
				size += pageSize;
			if ((flags & FLAGS_GUARD_PAGE_RIGHT))
				size += pageSize;
			if (!base)
			{
				flags &= ~FLAGS_ADDR_IS_HINT;
				base = FindBase(ctx, (flags & FLAGS_32BIT) ?  OBOS_PAGE_SIZE : OBOS_KERNEL_ADDRESS_SPACE_USABLE_BASE, (flags & FLAGS_32BIT) ? 0xffff'ffff : OBOS_KERNEL_ADDRESS_SPACE_LIMIT, size);
			}
			if (!CanAllocate(ctx, (void*)base, size))
			{
				if (flags & FLAGS_ADDR_IS_HINT)
				{
					base = FindBase(ctx, (flags & FLAGS_32BIT) ? OBOS_PAGE_SIZE : OBOS_KERNEL_ADDRESS_SPACE_USABLE_BASE, (flags & FLAGS_32BIT) ? 0xffff'ffff : OBOS_KERNEL_ADDRESS_SPACE_LIMIT, size);
					if (base)
						goto success;
				}
				return nullptr;
			}
		success:
			if (!ImplAllocatePages(ctx, (void*)base, size, protection, flags))
				return nullptr;
			
			if ((flags & FLAGS_GUARD_PAGE_LEFT))
				base += pageSize;
			return (void*)base;
		}
		bool Free(Context* ctx, void* base, size_t size)
		{
			if (!ctx)
				return false;
			if (!IsAllocated(ctx, base, size))
				return false;
			uintptr_t where = (uintptr_t)base;
			page_descriptor pd = {};
			arch::get_page_descriptor(ctx, (void*)where, pd);
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			size_t pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#else
			constexpr size_t pageSize = OBOS_PAGE_SIZE;
#endif
			if (where % pageSize)
				where -= (where % pageSize);
			if (size % pageSize)
				size += (pageSize - (size % pageSize));
			if (!size)
				return false;
			// Loop through the pages, un-mapping and freeing them.
			page_node* node = ctx->GetPageNode(base);
			OBOS_ASSERTP(node, "node is nullptr.");
			size_t i = 0;
			for (; i < node->nPageDescriptors; i++)
				if (InRange(node->pageDescriptors[i].virt,
					node->pageDescriptors[i].virt + (node->pageDescriptors[i].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
					base))
					break;
			OBOS_ASSERTP(InRange(node->pageDescriptors[i].virt,
				node->pageDescriptors[i].virt + (node->pageDescriptors[i].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
				base), "Could not find base in page descriptor table of node.\n");
			size_t basePd = i, endPd = 0;
			size_t expectedBasePdForBeginning = node->allocFlags & FLAGS_GUARD_PAGE_LEFT ? 1 : 0;
			size_t expectedEndPdForEnding = node->allocFlags & FLAGS_GUARD_PAGE_RIGHT ? node->nPageDescriptors - 2 : node->nPageDescriptors - 1;
			for (uintptr_t addr = where; addr < (where + size); addr += pageSize, i++)
			{
				arch::get_page_descriptor(ctx, (void*)addr, pd);
				if (!pd.present)
					continue;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
				pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#endif
				if (pd.phys && !pd.awaitingDemandPagingFault)
					arch::FreePhysicalPages(pd.phys, pageSize / OBOS_PAGE_SIZE);
				arch::unmap(ctx, (void*)addr);
			}
			endPd = i;
			if (basePd == expectedBasePdForBeginning && endPd == expectedEndPdForEnding)
			{
				ctx->RemovePageNode((void*)node->pageDescriptors[0].virt);
				return true;
			}
			page_descriptor* newPageDescriptors = new page_descriptor[node->nPageDescriptors-(endPd-basePd)];
			for (size_t i = 0; i < basePd; i++)
				newPageDescriptors[i] = node->pageDescriptors[i];
			for (size_t i = endPd; i < basePd; i++)
				newPageDescriptors[i - endPd] = node->pageDescriptors[i];
			delete[] node->pageDescriptors;
			node->pageDescriptors = newPageDescriptors;
			node->nPageDescriptors = (endPd - basePd);
			return true;
		}
	}
}