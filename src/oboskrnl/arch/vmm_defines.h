#pragma once

// Include platform defines.

#if defined(__x86_64__) || defined(_WIN64)
#include <arch/x86_64/vmm_defines.h>
#endif

// Check defines.
#ifndef OBOS_CHILDREN_PER_PT
#	error "Platform does not define OBOS_CHILDREN_PER_PT"
#endif
#ifndef OBOS_LEVELS_PER_PAGEMAP
#	error "Platform does not define OBOS_LEVELS_PER_PAGEMAP"
#endif
#ifndef OBOS_PAGE_SIZE
#	error "Platform does not define OBOS_PAGE_SIZE"
#endif
#ifndef OBOS_VIRT_ADDR_BITWIDTH
#	error "Platform does not define OBOS_VIRT_ADDR_BITWIDTH"
#endif
#ifndef OBOS_ADDR_BITWIDTH
#	error "Platform does not define OBOS_ADDR_BITWIDTH"
#endif
#ifndef OBOS_MAX_PAGE_FAULT_HANDLERS
#	error "Platform does not define OBOS_MAX_PAGE_FAULT_HANDLERS"
#endif
#ifndef OBOS_KERNEL_ADDRESS_SPACE_BASE
#	error "Platform does not define OBOS_KERNEL_ADDRESS_SPACE_BASE"
#endif
#ifndef OBOS_KERNEL_ADDRESS_SPACE_LIMIT
#	error "Platform does not define OBOS_KERNEL_ADDRESS_SPACE_LIMIT"
#endif
// We can define some macros on our own if they're obvious.
#ifdef OBOS_HUGE_PAGE_SIZE
#	define OBOS_HAS_HUGE_PAGE_SUPPORT 1
static_assert(OBOS_HUGE_PAGE_SIZE > OBOS_PAGE_SIZE, "Huge page size is less than the normal page size.");
#else
#	define OBOS_HAS_HUGE_PAGE_SUPPORT 0
#endif
#ifndef OBOS_IS_VIRT_ADDR_CANONICAL
#	include <todo.h>
COMPILE_MESSAGE("OBOS_IS_VIRT_ADDR_CANONICAL is not defined.\nConsider defining the macro. Default value is true.");
#	define OBOS_IS_VIRT_ADDR_CANONICAL(addr) (true)
#endif
#ifndef OBOS_ZERO_PAGE_PHYSICAL
#	define OBOS_ZERO_PAGE_PHYSICAL ((uintptr_t)0)
#endif
#ifndef OBOS_ADDRESS_SPACE_LIMIT
#	include <int.h>
#	define OBOS_ADDRESS_SPACE_LIMIT UINTPTR_MAX
#endif