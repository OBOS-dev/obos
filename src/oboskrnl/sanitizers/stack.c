/*
 * oboskrnl/sanitizers/stack.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

// Should be randomly set by kernel entry, but a default value will be provided anyway.
#if UINTPTR_MAX == UINT64_MAX
OBOS_EXPORT uint64_t __stack_chk_guard = 124770532977999;
#elif UINT32_MAX == UINTPTR_MAX
OBOS_EXPORT uint64_t __stack_chk_guard = 373612817;
#endif

OBOS_NORETURN OBOS_EXPORT void __stack_chk_fail()
{
	OBOS_Panic(OBOS_PANIC_STACK_CORRUPTION, "Stack corruption detected at IP=0x%p (overwrite of stack canary).\n", __builtin_extract_return_addr(__builtin_return_address(0)));
}