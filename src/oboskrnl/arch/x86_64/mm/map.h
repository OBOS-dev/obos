/*
	oboskrnl/arch/x86_64/mm/map.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace arch
	{
		void* map_page_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, uintptr_t prot);
		void* map_hugepage_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, uintptr_t prot);
		// This does not free the underlying physical page, but it does free the page structures.
		// Un-maps a page mapped with map_*
		void  unmap(class PageMap* pm, void* addr);
		uintptr_t get_page_phys(class PageMap* pm, void* addr);

		// nPages is in OBOS_PAGE_SIZE
		// So to allocate/free one OBOS_PAGE_SIZE, you would pass one as nPages.

		uintptr_t AllocatePhysicalPages(size_t nPages);
		void FreePhysicalPages(uintptr_t base, size_t nPages);

		void InitializePageTables();
	}
}