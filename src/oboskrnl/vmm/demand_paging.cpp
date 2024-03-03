/*
	oboskrnl/vmm/demand_paging.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <todo.h>

#include <vmm/init.h>
#include <vmm/page_node.h>

#include <arch/vmm_defines.h>
#include <arch/vmm_map.h>

namespace obos
{
	namespace vmm
	{
		void DemandPageHandler(void* on, PageFaultErrorCode errorCode, const page_descriptor& pd)
		{
			page_descriptor newPd = {};
			newPd.isHugePage = pd.isHugePage;
			newPd.virt = pd.virt;
			newPd.protFlags = pd.protFlags;
			newPd.present = true;
			if (!pd.isHugePage)
				arch::map_page_to((Context*)nullptr, newPd.virt, (newPd.phys = arch::AllocatePhysicalPages(1, false)), PROT_NO_DEMAND_PAGE);
			else
				arch::map_hugepage_to((Context*)nullptr, newPd.virt, (newPd.phys = arch::AllocatePhysicalPages(OBOS_HUGE_PAGE_SIZE / OBOS_PAGE_SIZE, true)), PROT_NO_DEMAND_PAGE);
			memzero((void*)newPd.virt, newPd.isHugePage ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
			if (!pd.isHugePage)
				arch::map_page_to((Context*)nullptr, newPd.virt, newPd.phys, pd.protFlags | PROT_NO_DEMAND_PAGE);
			else
				arch::map_hugepage_to((Context*)nullptr, newPd.virt, newPd.phys, pd.protFlags | PROT_NO_DEMAND_PAGE);
		}
	}
}