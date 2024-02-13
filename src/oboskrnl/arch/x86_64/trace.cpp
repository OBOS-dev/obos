/*
	oboskrnl/arch/x86_64/trace.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

namespace obos
{
	namespace logger
	{
		struct stack_frame
		{
			stack_frame* down;
			void* rip;
		};
		void stackTrace(void* parameter)
		{
			stack_frame* frame = parameter ? (stack_frame*)parameter : (stack_frame*)__builtin_frame_address(0);
			printf("Stack trace:\n");
			while (frame)
			{
				//const char* functionName = nullptr;
				//uintptr_t functionStart = 0;
				//if (functionName)
				//	printf("\t0x%p: %s+%d\n", frame->rip, functionName, (uintptr_t)frame->rip - functionStart);
				//else
				//	printf("\t0x%p: External Code\n", frame->rip);
				printf("\t0x%p: Function name unsupported.\n", frame->rip);
				frame = frame->down;
			}
		}
	}
}