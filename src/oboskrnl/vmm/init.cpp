/*
	oboskrnl/vmm/init.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <vmm/init.h>
#include <vmm/page_node.h>
#include <vmm/page_descriptor.h>

#include <allocators/basic_allocator.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

namespace obos
{
	namespace vmm
	{
		allocators::BasicAllocator g_generalKernelAllocator;
		allocators::BasicAllocator g_vmmAllocator;
		Context g_kernelContext;
		bool g_initialized;
		void DemandPageHandler(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd);
		void InitializeVMM()
		{
			logger::debug("%s: Initializing VMM allocator.\n", __func__);
			new (&g_vmmAllocator) allocators::BasicAllocator{};
			logger::debug("%s: Initializing general kernel allocator.\n", __func__);
			new (&g_generalKernelAllocator) allocators::BasicAllocator{};
			allocators::g_kAllocator = &g_generalKernelAllocator;
			logger::debug("%s: Registering demand page fault handler.\n", __func__);
			arch::register_page_fault_handler(PageFaultReason::PageFault_DemandPaging, false, DemandPageHandler);
			// Hopefully nothing bad will happen because the kernel allocator decided to use the kernel as it's data area.
			logger::debug("%s: Marking kernel as used memory.\n", __func__);
			page_node node{};
			node.ctx = &g_kernelContext;
			node.nPageDescriptors = (OBOS_KERNEL_TOP - OBOS_KERNEL_BASE) / OBOS_PAGE_SIZE;
			node.pageDescriptors = new page_descriptor[node.nPageDescriptors];
			size_t i = 0;
			for (uintptr_t addr = OBOS_KERNEL_BASE; addr < OBOS_KERNEL_TOP; addr += OBOS_PAGE_SIZE, i++)
				arch::get_page_descriptor(&g_kernelContext, (void*)addr, node.pageDescriptors[i]);
			g_kernelContext.AppendPageNode(node);
			arch::register_allocated_pages_in_context(&g_kernelContext);
			g_initialized = true;
		}
		static void* vmm_allocate(size_t count)
		{
			if (!g_vmmAllocator.GetAllocationSize())
				return g_vmmAllocator.Allocate(count);
			const size_t allocSize = g_vmmAllocator.GetAllocationSize();
			count = (count / allocSize) + ((count % allocSize) != 0);
			return g_vmmAllocator.Allocate(count);
		}
		void* page_descriptor::operator new(size_t count)
		{
			return vmm_allocate(count);
		}
		void* page_descriptor::operator new[](size_t count)
		{
			return vmm_allocate(count);
		}
		void* page_node::operator new(size_t count)
		{
			return vmm_allocate(count);
		}
		void* page_node::operator new[](size_t count)
		{
			return vmm_allocate(count);
		}
		static void vmm_free(void* ptr)
		{
			const size_t size = g_vmmAllocator.QueryObjectSize(ptr);
			g_vmmAllocator.Free(ptr, size);
		}
		void page_descriptor::operator delete (void* ptr) noexcept
		{
			vmm_free(ptr);
		}
		void page_descriptor::operator delete[](void* ptr) noexcept
		{
			vmm_free(ptr);
		}
		void page_descriptor::operator delete (void* ptr, size_t count) noexcept
		{
			vmm_free(ptr);
		}
		void page_descriptor::operator delete[](void* ptr, size_t size) noexcept
		{
			vmm_free(ptr);
		}
		void page_node::operator delete (void* ptr) noexcept
		{
			vmm_free(ptr);
		}
		void page_node::operator delete[](void* ptr) noexcept
		{
			vmm_free(ptr);
		}
		void page_node::operator delete (void* ptr, size_t count) noexcept
		{
			vmm_free(ptr);
		}
		void page_node::operator delete[](void* ptr, size_t size) noexcept
		{
			vmm_free(ptr);
		}
	}

}