/*
	oboskrnl/klog.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <console.h>
#include <stdarg.h>

#include <locks/spinlock.h>

#include <arch/smp_cpu_func.h>

#define STB_SPRINTF_NOFLOAT 1
#define STB_SPRINTF_IMPLEMENTATION 1
#define STB_SPRINTF_MIN 1
#include <external/stb_sprintf.h>

#if __x86_64__
#include <arch/x86_64/asm_helpers.h>
#endif

namespace obos
{
	namespace logger
	{
		static char* consoleOutputCallback(const char* buf, void*, int len)
		{
			for (size_t i = 0; i < (size_t)len; i++)
			{
				g_kernelConsole.ConsoleOutput(buf[i]);
#if __x86_64__
				outb(0xe9, buf[i]);
#endif
			}
			return (char*)buf;
		}
		static locks::SpinLock printf_lock;
		size_t printf(const char* format, ...)
		{
			printf_lock.Lock();
			va_list list;
			va_start(list, format);
			char ch = 0;
			size_t ret = stbsp_vsprintfcb(consoleOutputCallback, nullptr, &ch, format, list);
			va_end(list);
			printf_lock.Unlock();
			return ret;
		}
		size_t vprintf(const char* format, va_list list)
		{
			printf_lock.Lock();
			char ch = 0;
			size_t ret = stbsp_vsprintfcb(consoleOutputCallback, nullptr, &ch, format, list);
			printf_lock.Unlock();
			return ret;
		}
		size_t vsprintf(char* dest, const char* format, va_list list)
		{
			if (!dest)
				return stbsp_vsnprintf(dest, 0, format, list);
			return stbsp_vsprintf(dest, format, list);
		}
		size_t sprintf(char* dest, const char* format, ...)
		{
			if (!dest)
			{
				va_list list;
				va_start(list, format);
				size_t ret = stbsp_vsnprintf(dest, 0, format, list);
				va_end(list);
				return ret;
			}
			va_list list;
			va_start(list, format);
			size_t ret = stbsp_vsprintf(dest, format, list);
			va_end(list);
			return ret;
		}

		locks::SpinLock debug_lock;
		locks::SpinLock log_lock;
		locks::SpinLock warning_lock;
		locks::SpinLock error_lock;
#define __impl_log(colour, msg, lock_name) \
			lock_name.Lock();\
			Pixel oldForeground = 0;\
			Pixel oldBackground = 0;\
			g_kernelConsole.GetColour(oldForeground, oldBackground);\
			g_kernelConsole.SetColour((Pixel)colour, oldBackground);\
			while(*format == '\n') { printf("\n"); format++; } \
			va_list list; va_start(list, format);\
			size_t ret = printf(msg);\
			ret += vprintf(format, list);\
			va_end(list);\
			g_kernelConsole.SetColour(oldForeground, oldBackground);\
			lock_name.Unlock();\
			return ret

#ifdef OBOS_DEBUG
		size_t debug(const char* format, ...)
		{
			__impl_log(BLUE, DEBUG_PREFIX_MESSAGE, debug_lock);
			return 0;
		}
#else
		size_t debug(const char*, ...)
		{
			return 0;
		}
#endif
		size_t log(const char* format, ...)
		{
			__impl_log(GREEN, LOG_PREFIX_MESSAGE, log_lock);
		}
		size_t info(const char* format, ...)
		{
			__impl_log(GREEN, INFO_PREFIX_MESSAGE, log_lock);
		}
		size_t warning(const char* format, ...)
		{
			__impl_log(YELLOW, WARNING_PREFIX_MESSAGE, warning_lock);
		}
		size_t error(const char* format, ...)
		{
			__impl_log(ERROR_RED, ERROR_PREFIX_MESSAGE, error_lock);

		}
		[[noreturn]] void panic(void* stackTraceParameter, const char* format, ...)
		{
			va_list list;
			va_start(list, format);
			panicVariadic(stackTraceParameter, format, list);
			va_end(list);
			while (1);
		}
		[[noreturn]] void panicVariadic(void* stackTraceParameter, const char* format, va_list list)
		{
			arch::StopCPUs(false);
			g_kernelConsole.SetColour(GREY, PANIC_RED);
			g_kernelConsole.ClearConsole(PANIC_RED);
			g_kernelConsole.SetPosition(0, 0);
			vprintf(format, list);
			stackTrace(stackTraceParameter);
			while (1);
		}
	}
}