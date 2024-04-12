/*
	oboskrnl/arch/x86_64/kdbg/debugger_state.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/kdbg/bp.h>

#include <arch/x86_64/thr_context_info.h>

namespace obos
{
	namespace kdbg
	{
		struct debugger_state
		{
			bp* breakpoints[4];
			size_t nBreakpointsInUse;
			size_t nextBpIndex = 0;
		};
		extern debugger_state g_kdbgState;
		struct cpu_local_debugger_state
		{
			arch::ThreadContextInfo context;
			bool shouldStopAtNextInst : 1;
			bool isFinishingFunction : 1;
			size_t nCallsSinceFinishCommand = 0;
		};
	}

}