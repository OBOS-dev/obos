/*
	oboskrnl/arch/x86_64/mm/map.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vmm/prot.h>
#include <vmm/page_descriptor.h>
#include <vmm/page_fault_reason.h>

namespace obos
{
	namespace arch
	{
		// External interface.

		void* map_page_to(uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		void* map_hugepage_to(uintptr_t virt, uintptr_t phys, vmm::prot_t prot);

		void unmap(void* addr);
		uintptr_t get_page_phys(void* addr);
		void get_page_descriptor(void* addr, vmm::page_descriptor& out);
		
		bool register_page_fault_handler(vmm::PageFaultReason reason, bool hasToBeInUserMode, void(*callback)(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd));

		// nPages is in OBOS_PAGE_SIZE
		// So to allocate/free one OBOS_PAGE_SIZE, you would pass one as nPages.

		uintptr_t AllocatePhysicalPages(size_t nPages);
		void FreePhysicalPages(uintptr_t base, size_t nPages);

		// Internal interface.

		void* map_page_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		void* map_hugepage_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		// This does not free the underlying physical page, but it does free the page structures.
		// Un-maps a page mapped with map_*
		void  unmap(class PageMap* pm, void* addr);
		uintptr_t get_page_phys(class PageMap* pm, void* addr);
		void get_page_descriptor(class PageMap* pm, void* addr, vmm::page_descriptor& out);

		void InitializePageTables();
	}
}