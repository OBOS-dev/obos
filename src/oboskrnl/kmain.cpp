/*
	oboskrnl/main.cpp
 
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

namespace obos
{
	void kmain()
	{
		logger::debug("In %s.\n", __func__);
		while(1);
	}
}

#if UINT32_MAX == UINTPTR_MAX
#define STACK_CHK_GUARD 0xe2dee396
#else
#define STACK_CHK_GUARD 0x1C747501613CB3
#endif
 
extern "C" uintptr_t __stack_chk_guard = STACK_CHK_GUARD;
 
extern "C" [[noreturn]] void __stack_chk_fail(void)
{
	obos::logger::panic(nullptr, "Stack corruption detected!\n");
}