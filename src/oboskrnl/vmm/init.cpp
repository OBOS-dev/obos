/*
	oboskrnl/vmm/init.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <klog.h>

#include <vmm/init.h>
#include <vmm/page_node.h>

#include <allocators/slab.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

namespace obos
{
	namespace vmm
	{
		allocators::SlabAllocator g_pgNodeAllocator;
		allocators::SlabAllocator g_pdAllocator;
		allocators::SlabAllocator g_generalKernelAllocator;
		Context g_kernelContext;
		bool g_initialized;
		static char s_allocatorBootstrapper[0x8'0000];
		void DemandPageHandler(void* on, vmm::PageFaultErrorCode errorCode, const vmm::page_descriptor& pd);
		void InitializeVMM()
		{
			logger::debug("%s: Initializing page node allocator.\n", __func__);
			new (&g_pgNodeAllocator) allocators::SlabAllocator{};
			g_pgNodeAllocator.Initialize(nullptr, sizeof(page_node), false, 0, 16);
			g_pgNodeAllocator.AddRegion(s_allocatorBootstrapper + 0, 0x4'0000);
			logger::debug("%s: Initializing page descriptor allocator.\n", __func__);
			new (&g_pdAllocator) allocators::SlabAllocator{};
			g_pdAllocator.Initialize(nullptr, sizeof(page_descriptor), false, 0,16);
			g_pdAllocator.AddRegion(s_allocatorBootstrapper + 0x4'0000, 0x4'0000);
			logger::debug("%s: Marking kernel as used memory.\n", __func__);
			page_node node{};
			node.ctx = &g_kernelContext;
			node.nPageDescriptors = (OBOS_KERNEL_TOP - OBOS_KERNEL_BASE) / OBOS_PAGE_SIZE;
			node.pageDescriptors = (page_descriptor*)g_pdAllocator.Allocate(node.nPageDescriptors);
			size_t i = 0;
			for (uintptr_t addr = OBOS_KERNEL_BASE; addr < OBOS_KERNEL_TOP; addr += OBOS_PAGE_SIZE, i++)
				arch::get_page_descriptor(&g_kernelContext, (void*)addr, node.pageDescriptors[i]);
			g_kernelContext.AppendPageNode(node);
			logger::debug("%s: Registering demand page fault handler.\n", __func__);
			arch::register_page_fault_handler(PageFaultReason::PageFault_DemandPaging, false, DemandPageHandler);
			logger::debug("%s: Initializing general kernel allocator.\n", __func__);
			new (&g_generalKernelAllocator) allocators::SlabAllocator{};
			g_generalKernelAllocator.Initialize(nullptr, 0x10, true, 0x1000, 16);
			allocators::g_kAllocator = &g_generalKernelAllocator;
			g_initialized = true;
		}
	}
}