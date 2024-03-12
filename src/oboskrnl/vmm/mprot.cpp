/*
	oboskrnl/vmm/mprot.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <todo.h>
#include <memmanip.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>
#include <vmm/prot.h>
#include <vmm/mprot.h>
#include <vmm/page_node.h>
#include <vmm/page_descriptor.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

namespace obos
{
	namespace vmm
	{
		static bool _InRange(uintptr_t base, uintptr_t end, uintptr_t val)
		{
			return (val >= base) && (val < end);
		}
#define InRange(base, end, val) (obos::vmm::_InRange((uintptr_t)(base), (uintptr_t)(end), (uintptr_t)(val)))
		// Defined in map.cpp
		extern bool IsAllocated(Context* ctx, void* base, size_t size);
		TODO("Test SetProtection and GetPageDescriptors.")
		bool SetProtection(Context* ctx, void* base, size_t size, prot_t protection)
		{
			// Overview of what this function does:
			// 1. It verifies all these pages are at least reserved.
			// 2. If so, it iterates over all of the pages, and for each page, it sets the protection in the page node and in the page tables.
			// 3. Then it returns.
			if (!IsAllocated(ctx, base, size) || !ctx)
				return false;
			if ((uintptr_t)base % OBOS_PAGE_SIZE)
				base = (void*)((uintptr_t)base - ((uintptr_t)base % OBOS_PAGE_SIZE));
			if (size % OBOS_PAGE_SIZE)
				size += (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			size_t ps = 0;
#else
			constexpr size_t ps = OBOS_PAGE_SIZE;
#endif
			page_node* node = ctx->GetPageNode(base);
			size_t pdI = 0;
			for (; pdI < node->nPageDescriptors; pdI++)
				if (InRange(node->pageDescriptors[pdI].virt,
#if OBOS_HAS_HUGE_PAGE_SUPPORT
							node->pageDescriptors[pdI].virt + (node->pageDescriptors[pdI].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
#else
							node->pageDescriptors[pdI].virt + OBOS_PAGE_SIZE,
#endif
							base))
					break;
			protection &= ~PROT_NO_DEMAND_PAGE;
			for (uintptr_t where = (uintptr_t)base; where < ((uintptr_t)base + size); where += ps)
			{
				page_descriptor& pd = node->pageDescriptors[pdI++];
				pd.protFlags = protection;
				page_descriptor realPd{};
				arch::get_page_descriptor(ctx, (void*)where, realPd);
				if(!realPd.awaitingDemandPagingFault)
					pd.protFlags |= PROT_NO_DEMAND_PAGE;
				// No need for the other case, as it's impossible for protection to have the PROT_NO_DEMAND_PAGE bit set (see the line(s) above this for loop).
				//else
				//	pd.protFlags &= ~PROT_NO_DEMAND_PAGE;
				pd.awaitingDemandPagingFault = realPd.awaitingDemandPagingFault;
				pd.phys = realPd.phys;
				if (pd.present)
					!pd.isHugePage ? arch::map_page_to(ctx, pd.virt, pd.phys, pd.protFlags) : arch::map_hugepage_to(ctx, pd.virt, pd.phys, pd.protFlags);
#if OBOS_HAS_HUGE_PAGE_SUPPORT
				ps = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#endif
			}
			return true;
		}
		size_t GetPageDescriptor(Context* ctx, void* base, size_t size, page_descriptor* oArr, size_t maxElements)
		{
			if (!ctx)
				return SIZE_MAX;
			page_node* node = ctx->GetPageNode(base);
			if (!node)
			{
				size_t i = 0;
				for (uintptr_t where = (uintptr_t)base; where < ((uintptr_t)base + size); where += OBOS_PAGE_SIZE, i++)
				{
					if (i < maxElements)
					{
						memzero(oArr + i, sizeof(oArr[i]));
						oArr[i].virt = where;
					}
				}
				return (maxElements >= i) ? 0 : i - maxElements;
			}
			size_t pdI = 0;
			for (; pdI < node->nPageDescriptors; pdI++)
				if (InRange(node->pageDescriptors[pdI].virt,
#if OBOS_HAS_HUGE_PAGE_SUPPORT
					node->pageDescriptors[pdI].virt + (node->pageDescriptors[pdI].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE),
#else
					node->pageDescriptors[pdI].virt + OBOS_PAGE_SIZE,
#endif
					base))
					break;
			size_t i = 0;
#if OBOS_HAS_HUGE_PAGE_SUPPORT
			size_t sz = 0;
#else
			constexpr size_t sz = OBOS_PAGE_SIZE;
#endif
			for (uintptr_t where = (uintptr_t)base; where < ((uintptr_t)base + size); where += sz, i++, pdI++)
			{
				page_descriptor realPd{};
				arch::get_page_descriptor(ctx, (void*)where, realPd);
				node->pageDescriptors[pdI].awaitingDemandPagingFault = realPd.awaitingDemandPagingFault;
				if (i < maxElements)
					oArr[i] = node->pageDescriptors[pdI];
				sz = node->pageDescriptors[pdI].isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
			}
			return (maxElements >= i) ? 0 : i - maxElements;
		}
	}
}