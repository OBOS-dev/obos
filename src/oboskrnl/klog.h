/*
	oboskrnl/klog.h

	Copyright (c) 2023-2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <stdarg.h>

#ifdef OBOS_DEBUG
#define OBOS_ASSERTP(expr, msg, ...) if (!(expr)) { obos::logger::panic(nullptr, "Function %s, File %s, Line %d: Assertion failed, \"%s\". " msg "\n", __func__, __FILE__, __LINE__, #expr __VA_ARGS__); }
#define OBOS_ASSERT(expr, msg, ...) if (!(expr)) { obos::logger::error("Function %s, File %s, Line %d: Assertion failed, \"%s\". " msg "\n", __func__, __FILE__, __LINE__, #expr __VA_ARGS__); }
#else
#define OBOS_ASSERTP(expr, msg, ...)
#define OBOS_ASSERT(expr, msg, ...)
#endif

namespace obos
{
	namespace logger
	{
		enum
		{
			GREY = 0x00D3D3D3,
			GREEN = 0x0003D12B,
			BLUE = 0x00566F84,
			YELLOW = 0x00ffcc00,
			ERROR_RED = 0x00cc3300,
			PANIC_RED = 0x00ac1616,
		};

		size_t printf(const char* format, ...);
		size_t vprintf(const char* format, va_list list);
		size_t vsprintf(char* dest, const char* format, va_list list);
		size_t sprintf(char* dest, const char* format, ...);

		constexpr const char* DEBUG_PREFIX_MESSAGE = "[Debug] ";
		constexpr const char* LOG_PREFIX_MESSAGE = "[Log] ";
		constexpr const char* INFO_PREFIX_MESSAGE = "[Log] ";
		constexpr const char* WARNING_PREFIX_MESSAGE = "[Warning] ";
		constexpr const char* ERROR_PREFIX_MESSAGE = "[Error] ";

		size_t debug(const char* format, ...);
		size_t log(const char* format, ...);
		size_t info(const char* format, ...);
		size_t warning(const char* format, ...);
		size_t error(const char* format, ...);
		[[noreturn]] void panic(void* stackTraceParameter, const char* format, ...);
		[[noreturn]] void panicVariadic(void* stackTraceParameter, const char* format, va_list list);

		void stackTrace(void* stackTraceParameter);
	}
}