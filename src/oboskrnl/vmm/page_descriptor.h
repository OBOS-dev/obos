/*
	oboskrnl/vmm/page_descriptor.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <vmm/prot.h>

namespace obos
{
	namespace vmm
	{
		struct page_descriptor
		{
			uintptr_t virt;
			uintptr_t phys;
			prot_t protFlags;
			bool isHugePage : 1;
			bool present : 1;
			bool awaitingDemandPagingFault : 1;
			void* operator new(size_t count);
			void* operator new[](size_t count);
		} OBOS_ALIGN(4);
	}
}