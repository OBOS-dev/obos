/*
	oboskrnl/klog.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <stdarg.h>

#define OBOS_ASSERT(expression) do { if (!(expression)) { OBOS_Panic(OBOS_PANIC_ASSERTION_FAILED, "Assertion failed in function %s. File: %s, line %d. %s\n", __func__, __FILE__, __LINE__, #expression); } } while(0)

typedef enum
{
	OBOS_PANIC_EXCEPTION,
	OBOS_PANIC_FATAL_ERROR,
	OBOS_PANIC_KASAN_VIOLATION,
	OBOS_PANIC_UBSAN_VIOLATION,
	OBOS_PANIC_DRIVER_FAILURE,
	OBOS_PANIC_ASSERTION_FAILED,
	OBOS_PANIC_SCHEDULER_ERROR,
	OBOS_PANIC_NO_MEMORY,
} panic_reason;
typedef enum
{
	LOG_LEVEL_DEBUG,
	LOG_LEVEL_LOG,
	LOG_LEVEL_WARNING,
	LOG_LEVEL_ERROR,
} log_level;
/// <summary>
/// Sets the current log level.
/// </summary>
/// <param name="level">The new log level.</param>
void OBOS_SetLogLevel(log_level level);
/// <summary>
/// Gets the current log level.
/// </summary>
/// <returns>The current log level.</returns>
log_level OBOS_GetLogLevel();
/// <summary>
/// Prints a debug log.
/// </summary>
/// <param name="format">The printf-style format string.</param>
/// <param name="...">Any variadic arguments.</param>
void OBOS_Debug(const char* format, ...);
/// <summary>
/// Prints a general log.
/// </summary>
/// <param name="format">The printf-style format string.</param>
/// <param name="...">Any variadic arguments.</param>
void OBOS_Log(const char* format, ...);
/// <summary>
/// Prints a warning.
/// </summary>
/// <param name="format">The printf-style format string.</param>
/// <param name="...">Any variadic arguments.</param>
void OBOS_Warning(const char* format, ...);
/// <summary>
/// Prints a non-fatal error.
/// </summary>
/// <param name="format">The printf-style format string.</param>
/// <param name="...">Any variadic arguments.</param>
void OBOS_Error(const char* format, ...);
/// <summary>
/// Panics.
/// </summary>
/// <param name="reason">The reason for the panic.</param>
/// <param name="format">The printf-style format string describing how the fail happened..</param>
/// <param name="...">Any variadic arguments.</param>
OBOS_NORETURN void OBOS_Panic(panic_reason reason, const char* format, ...);

// printf-Style functions.
size_t printf(const char* format, ...);
size_t vprintf(const char* format, va_list list);
size_t snprintf(char* buf, size_t bufSize, const char* format, ...);
size_t vsnprintf(char* buf, size_t bufSize, const char* format, va_list list);