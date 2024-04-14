/*
	oboskrnl/arch/x86_64/mm/map.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

#include <vmm/prot.h>
#include <vmm/page_descriptor.h>
#include <vmm/page_fault_reason.h>
#include <vmm/pg_context.h>

namespace obos
{
	namespace arch
	{
		// External interface.

		/// <summary>
		/// Maps a physical page to a virtual address.
		/// </summary>
		/// <param name="ctx">The context. This can be nullptr to specify the current context.</param>
		/// <param name="virt">The virtual address to map to.</param>
		/// <param name="phys">The physical page to map.</param>
		/// <param name="prot">The protection flags.</param>
		/// <returns>The virtual address.</returns>
		OBOS_EXPORT void* map_page_to(vmm::Context* ctx, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		/// <summary>
		/// Maps a physical page to a virtual address as a huge page.
		/// </summary>
		/// <param name="ctx">The context. This can be nullptr to specify the current context.</param>
		/// <param name="virt">The virtual address to map to.</param>
		/// <param name="phys">The physical page to map.</param>
		/// <param name="prot">The protection flags.</param>
		/// <returns>The virtual address.</returns>
		OBOS_EXPORT void* map_hugepage_to(vmm::Context* ctx, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);

		/// <summary>
		/// Un-maps a page.
		/// </summary>
		/// <param name="ctx">The context. This can be nullptr to specify the current context.</param>
		/// <param name="addr">The address to un-map. The underlying physical page is kept allocated.</param>
		OBOS_EXPORT void unmap(vmm::Context* ctx, void* addr);
		/// <summary>
		/// Makes a page descriptor from a virtual page.
		/// </summary>
		/// <param name="ctx">The context. This can be nullptr to specify the current context.</param>
		/// <param name="addr">The virtual address to make a page descriptor from.</param>
		/// <param name="out">[out] The page descriptor to store the result in.</param>
		OBOS_EXPORT void get_page_descriptor(vmm::Context* ctx, void* addr, vmm::page_descriptor& out);

		/// <summary>
		/// Registers any allocated pages that InitializeVMM() might not know about in the kernel context.<br>
		/// This is probably only to be called once in InitializeVMM().
		/// </summary>
		/// <param name="ctx">The context.</param>
		void register_allocated_pages_in_context(vmm::Context* ctx);

		/// <summary>
		/// Registers a page fault handler.
		/// </summary>
		/// <param name="reason">The reason that triggers the callback.</param>
		/// <param name="hasToBeInUserMode">Whether the fault has to be from user-mode thread.</param>
		/// <param name="callback">The handler.</param>
		/// <returns>Whether the handler was registered (true) or not (false).</returns>
		OBOS_EXPORT bool register_page_fault_handler(vmm::PageFaultReason reason, bool hasToBeInUserMode, void(*callback)(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd));

		// nPages is in units of OBOS_PAGE_SIZE.
		// So to allocate/free one OBOS_PAGE_SIZE, you would pass one as nPages.
		// As a note for anyone using x86-64 as a reference, these rules must be followed, or you'll have a bad time.

		/// <summary>
		/// Allocates physical pages.
		/// </summary>
		/// <param name="nPages">The number of pages to allocate.</param>
		/// <param name="alignToHugePageSize">Whether to align to the huge page size (true), or not (false).</param>
		/// <returns>The physical address of the pages allocated.</returns>
		OBOS_EXPORT uintptr_t AllocatePhysicalPages(size_t nPages, bool alignToHugePageSize);
		/// <summary>
		/// Frees physical pages.
		/// </summary>
		/// <param name="base">The base of the pages.</param>
		/// <param name="nPages">The amount of pages to free.</param>
		OBOS_EXPORT void FreePhysicalPages(uintptr_t base, size_t nPages);

		// Internal interface.

		void* map_page_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		void* map_hugepage_to(class PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot);
		// This does not free the underlying physical page, but it does free the page structures.
		// Un-maps a page mapped with map_*
		void  unmap(class PageMap* pm, void* addr);
		void get_page_descriptor(class PageMap* pm, void* addr, vmm::page_descriptor& out);

		/// <summary>
		/// Initializes the page tables of the kernel.
		/// This sets up the HHDM, kernel memory, and framebuffer.
		/// </summary>
		void InitializePageTables();
	}
}