/*
	oboskrnl/arch/x86_64/kdbg/init.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <arch/x86_64/kdbg/init.h>
#include <arch/x86_64/kdbg/io.h>

#include <locks/spinlock.h>

namespace obos
{
	namespace kdbg
	{
		bool g_initialized;
		void init_kdbg(input_format inputDev, output_format outputDev)
		{
#if OBOS_KDBG_ENABLED
			g_inputDev = inputDev;
			g_outputDev = outputDev;
			g_initialized = true;
			printf("oboskrnl: Kernel debugger is on.\n");
			breakpoint();
#endif
		}
	}
}