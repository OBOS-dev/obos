/*
	oboskrnl/arch/x86_64/vmm_defines.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#define OBOS_LEVELS_PER_PAGEMAP (4)
#define OBOS_CHILDREN_PER_PT (512)
#define OBOS_PAGE_SIZE (4096)
#define OBOS_HUGE_PAGE_SIZE (2097152)
#define OBOS_IS_VIRT_ADDR_CANONICAL(addr) (((uintptr_t)(addr) >> 47) == 0 || ((uintptr_t)(addr) >> 47) == 0x1ffff)
#define OBOS_VIRT_ADDR_BITWIDTH (48)
#define OBOS_ADDR_BITWIDTH (64)
#define OBOS_ZERO_PAGE_PHYSICAL ((uintptr_t)0)
#define OBOS_MAX_PAGE_FAULT_HANDLERS (32)
namespace obos
{
	namespace vmm
	{
		enum
		{
			PROT_x86_64_WRITE_COMBINING_CACHE = 0x0100'0000,
			PROT_x86_64_WRITE_THROUGH_CACHE = 0x0200'0000,
		};
	}
}