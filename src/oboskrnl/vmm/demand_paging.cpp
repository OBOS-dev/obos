/*
	oboskrnl/vmm/demand_paging.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
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
		OBOS_NO_KASAN OBOS_NO_UBSAN void DemandPageHandler(void*, PageFaultErrorCode, const page_descriptor& pd)
		{
#ifdef OBOS_DEBUG
			OBOS_ASSERTP(pd.awaitingDemandPagingFault, "Demand page handler called for no reason.");
#endif
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
#ifdef OBOS_DEBUG
			arch::get_page_descriptor((Context*)nullptr, (void*)newPd.virt, newPd);
			OBOS_ASSERTP(!newPd.awaitingDemandPagingFault, "Changes to page descriptor for virtual address 0x%p did not go through.\n",, (void*)newPd.virt);
#endif
		}
	}
}