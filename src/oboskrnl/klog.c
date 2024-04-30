/*
	oboskrnl/klog.h

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <stdarg.h>

#define STB_SPRINTF_NOFLOAT 1
#define STB_SPRINTF_IMPLEMENTATION 1
#define STB_SPRINTF_MIN 8
#include <external/stb_sprintf.h>

#ifdef __x86_64__
#	include <arch/x86_64/asm_helpers.h>
#endif

static log_level s_logLevel;
void OBOS_SetLogLevel(log_level level)
{
	s_logLevel = level;
}
log_level OBOS_GetLogLevel()
{
	return s_logLevel;
}
static void common_log(log_level minimumLevel, const char* log_prefix, const char* format, va_list list)
{
	if (s_logLevel > minimumLevel)
		return;
	printf("[ %s ] ", log_prefix);
	vprintf(format, list);
}
void OBOS_Debug(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_DEBUG, "DEBUG", format, list);
	va_end(list);
}
void OBOS_Log(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_LOG, "LOG", format, list);
	va_end(list);
}
void OBOS_Warning(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_WARNING, "WARN", format, list);
	va_end(list);
}
void OBOS_Error(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_ERROR, "ERROR", format, list);
	va_end(list);
}
OBOS_NORETURN void OBOS_Panic(panic_reason reason, const char* format, ...)
{
	(reason = reason);
	const char* ascii_art =
"  _____ _______ ____  _____  \n"
" / ____|__   __/ __ \\|  __ \\ \n"
"| (___    | | | |  | | |__) |\n"
" \\___ \\   | | | |  | |  ___/ \n"
" ____) |  | | | |__| | |     \n"
"|_____/   |_|  \\____/|_|     ";


	printf("\n%s\n", ascii_art);
	printf("Kernel Panic! Reason: %d. Information on the crash is below:\n");
	va_list list;
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	while (1);
}

static char* outputCallback(const char* buf, void* a, int len)
{
	for (size_t i = 0; i < (size_t)len; i++)
	{
#ifdef __x86_64__
		outb(0xE9, buf[i]);
#endif
	}
	return (char*)buf;
}
size_t printf(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	size_t ret = vprintf(format, list);
	va_end(list);
	return ret;
}
size_t vprintf(const char* format, va_list list)
{
	char ch[8];
	return stbsp_vsprintfcb(outputCallback, nullptr, ch, format, list);
}
size_t snprintf(char* buf, size_t bufSize, const char* format, ...)
{
	va_list list;
	va_start(list, format);
	size_t ret = vsnprintf(buf, bufSize, format, list);
	va_end(list);
	return ret;
}
size_t vsnprintf(char* buf, size_t bufSize, const char* format, va_list list)
{
	return stbsp_vsnprintf(buf, bufSize, format, list);
}