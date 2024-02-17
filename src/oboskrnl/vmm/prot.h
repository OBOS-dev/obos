/*
	oboskrnl/vmm/prot.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vmm
	{
		enum
		{
			PROT_READ_ONLY = 0x1,
			PROT_EXECUTE = 0x2,
			PROT_USER = 0x4,
			PROT_CACHE_DISABLE = 0x8,
			PROT_NO_DEMAND_PAGE = 0x10,
		};
		enum
		{
			FLAGS_32BIT = 0x1,
			FLAGS_GUARD_PAGE = 0x2,
			FLAGS_USE_HUGE_PAGES = 0x4,
			FLAGS_HINT = 0x8,
		};
	}
}