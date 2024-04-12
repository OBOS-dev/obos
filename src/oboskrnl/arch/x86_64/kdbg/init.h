/*
	oboskrnl/arch/x86_64/kdbg/init.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/kdbg/io.h>

#if OBOS_KDBG_ENABLED
#define breakpoint() asm volatile("int3")
#else
#define breakpoint() do {} while(0)
#endif

namespace obos
{
	namespace kdbg
	{
		extern bool g_initialized;
		void init_kdbg(input_format inputDev, output_format outputDev);
	}

}