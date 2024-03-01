/*
	oboskrnl/vmm/map.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <todo.h>

#include <arch/vmm_map.h>
#include <arch/vmm_defines.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>
#include <vmm/prot.h>
#include <vmm/map.h>

TODO("Implement demand paging.");

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
			bool allocateHugePages = (flags & FLAGS_USE_HUGE_PAGES);
#if OBOS_HAS_HUGE_PAGE_SUPPORT == 0
			COMPILE_MESSAGE("OBOS_HAS_HUGE_PAGE_SUPPORT being zero hasn't tested, remove this on the first architecture (after fixing any bugs) that doesn't implement huge pages.\n");
			if (allocateHugePages)
				return nullptr;
#endif
			FIXME("Disable no demand paging flag by default.")
			protection |= PROT_NO_DEMAND_PAGE;
			size_t pageSize = allocateHugePages ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
			uintptr_t where = (uintptr_t)_where;
			where -= (where % pageSize);
			size += (size - (size % pageSize));
			if (!where || !size)
				return nullptr;
			// Loop through the pages, mapping them.
			if ((flags & FLAGS_DISABLE_HUGEPAGE_OPTIMIZATION) && !allocateHugePages)
			{
				for (uintptr_t addr = where; addr < (where + size); addr += pageSize)
					arch::map_page_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(1, false), protection);
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
					arch::map_page_to((Context*)nullptr, addr, arch::AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE / OBOS_PAGE_SIZE, true), protection);
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
			where -= (where % pageSize);
			size += (size - (size % pageSize));
			if (!where || !size)
				return false;
			// Loop through the pages, un-mapping and freeing them.
			for (uintptr_t addr = where; addr < (where + size); addr += pageSize)
			{
				arch::get_page_descriptor((Context*)nullptr, (void*)addr, pd);
				if (!pd.present)
				{
#if !OBOS_HAS_HUGE_PAGE_SUPPORT
					pageSize = OBOS_PAGE_SIZE;
#endif
					break;
				}
#if OBOS_HAS_HUGE_PAGE_SUPPORT
				pageSize = pd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
#endif
				arch::FreePhysicalPages(pd.phys, pageSize / OBOS_PAGE_SIZE);
				arch::unmap((Context*)nullptr, (void*)addr);
			}
			return true;
		}

		bool MapPageDescriptor(Context* ctx, const page_descriptor& pd)
		{

		}
		void* Allocate(Context* ctx, void* base, size_t size, allocflag_t flags, prot_t protection)
		{

		}
		bool Free(Context* ctx, void* base, size_t size)
		{

		}
	}
}