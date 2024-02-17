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