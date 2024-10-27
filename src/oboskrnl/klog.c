/*
	oboskrnl/klog.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <stdarg.h>
#include <memmanip.h>
#include <text.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

#include <scheduler/process.h>

#include <irq/irql.h>

#define STB_SPRINTF_NOFLOAT 1
#define STB_SPRINTF_IMPLEMENTATION 1
#define STB_SPRINTF_MIN 8
#include <external/stb_sprintf.h>

#ifdef __x86_64__
#	include <arch/x86_64/asm_helpers.h>
#elif defined(__m68k__)
#	include <arch/x86_64/pmm.h>
#endif

const char* OBOSH_PanicReasonToStr(panic_reason reason)
{
	static const char* const table[] = {
		"OBOS_PANIC_EXCEPTION",
		"OBOS_PANIC_FATAL_ERROR",
		"OBOS_PANIC_KASAN_VIOLATION",
		"OBOS_PANIC_UBSAN_VIOLATION",
		"OBOS_PANIC_DRIVER_FAILURE",
		"OBOS_PANIC_ASSERTION_FAILED",
		"OBOS_PANIC_SCHEDULER_ERROR",
		"OBOS_PANIC_NO_MEMORY",
		"OBOS_PANIC_ALLOCATOR_ERROR",
		"OBOS_PANIC_STACK_CORRUPTION",
	};
	if (reason > OBOS_PANIC_STACK_CORRUPTION)
		return nullptr;
	return table[reason];
}
static log_level s_logLevel;
OBOS_PAGEABLE_FUNCTION void OBOS_SetLogLevel(log_level level)
{
	s_logLevel = level;
}
OBOS_EXPORT log_level OBOS_GetLogLevel()
{
	return s_logLevel;
}
static spinlock s_loggerLock; bool s_loggerLockInitialized = false;
static spinlock s_printfLock; bool s_printfLockInitialized = false;
color OBOS_LogLevelToColor[LOG_LEVEL_NONE] = {
	COLOR_LIGHT_BLUE,
	COLOR_LIGHT_GREEN,
	COLOR_YELLOW,
	COLOR_RED,
};
#define CALLBACK_COUNT (8)
static log_backend outputCallbacks[CALLBACK_COUNT];
static size_t nOutputCallbacks;
static void common_log(log_level minimumLevel, const char* log_prefix, const char* format, va_list list)
{
	if (!s_loggerLockInitialized)
	{
		s_loggerLock = Core_SpinlockCreate();
		s_loggerLockInitialized = true;
	}
	if (s_logLevel > minimumLevel)
		return;
	irql oldIrql = Core_SpinlockAcquire(&s_loggerLock);
	OBOS_SetColor(OBOS_LogLevelToColor[minimumLevel]);
	printf("[ %s ] ", log_prefix);
	vprintf(format, list);
	OBOS_ResetColor();
	Core_SpinlockRelease(&s_loggerLock, oldIrql);
}
void OBOS_SetColor(color c)
{
	for (size_t i = 0; i < nOutputCallbacks; i++)
		if (outputCallbacks[i].set_color)
			outputCallbacks[i].set_color(c, outputCallbacks[i].userdata);
}
void OBOS_ResetColor()
{
	for (size_t i = 0; i < nOutputCallbacks; i++)
		if (outputCallbacks[i].reset_color)
			outputCallbacks[i].reset_color(outputCallbacks[i].userdata);
}
OBOS_EXPORT void OBOS_Debug(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_DEBUG, "DEBUG", format, list);
	va_end(list);
}
OBOS_EXPORT void OBOS_Log(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_LOG, " LOG ", format, list);
	va_end(list);
}
OBOS_EXPORT void OBOS_Warning(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_WARNING, "WARN ", format, list);
	va_end(list);
}
OBOS_EXPORT void OBOS_Error(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	common_log(LOG_LEVEL_ERROR, "ERROR", format, list);
	va_end(list);
}
static uint32_t getCPUId()
{
	if (!CoreS_GetCPULocalPtr())
		return (uint32_t)0;
	return CoreS_GetCPULocalPtr()->id;
}
static uint32_t getTID()
{
	if (!CoreS_GetCPULocalPtr())
		return (uint32_t)-1;
	if (!CoreS_GetCPULocalPtr()->currentThread)
		return (uint32_t)-1;
	return CoreS_GetCPULocalPtr()->currentThread->tid;
}
static uint32_t getPID()
{
	if (!CoreS_GetCPULocalPtr())
		return (uint32_t)-1;
	if (!CoreS_GetCPULocalPtr()->currentThread)
		return (uint32_t)-1;
	if (!CoreS_GetCPULocalPtr()->currentThread->proc)
		return (uint32_t)-1;
	return CoreS_GetCPULocalPtr()->currentThread->proc->pid;
}
OBOS_NORETURN OBOS_NO_KASAN OBOS_EXPORT void OBOS_Panic(panic_reason reason, const char* format, ...)
{
	const char* ascii_art =
		"       )\r\n"
        "    ( /(                        (\r\n"
        "    )\\())  (   (             (  )\\             )        (\r\n"
        "   ((_)\\  ))\\  )(    (      ))\\((_)  `  )   ( /(   (    )\\   (\r\n"
        "  (_ ((_)/((_)(()\\   )\\ )  /((_)_    /(/(   )(_))  )\\ )((_)  )\\\r\n"
        "  | |/ /(_))   ((_) _(_/( (_)) | |  ((_)_\\ ((_)_  _(_/( (_) ((_)\r\n"
        "  | ' < / -_) | '_|| ' \\))/ -_)| |  | '_ \\)/ _` || ' \\))| |/ _|\r\n"
        "  |_|\\_\\\\___| |_|  |_||_| \\___||_|  | .__/ \\__,_||_||_| |_|\\__|\r\n"
        "                                    |_|\r\n";
#ifndef OBOS_UP
	if (OBOSS_HaltCPUs)
		OBOSS_HaltCPUs();
#endif
/*	Core_SpinlockForcedRelease(&s_printfLock);
	Core_SpinlockForcedRelease(&s_loggerLock);*/
	Core_SpinlockRelease(&s_printfLock, Core_GetIrql());
	Core_SpinlockRelease(&s_loggerLock, Core_GetIrql());
	OBOS_TextRendererState.fb.backbuffer_base = nullptr; // the back buffer might cause some trouble.
	irql oldIrql = Core_RaiseIrqlNoThread(IRQL_MASKED);
	OBOS_UNUSED(oldIrql);
	OBOS_ResetColor();
	printf("\n%s\n", ascii_art);
	printf("Kernel Panic on CPU %d in thread %d, owned by process %d. Reason: %s. Information on the crash is below:\n", getCPUId(), getTID(), getPID(), OBOSH_PanicReasonToStr(reason));
	va_list list;
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	while (1)
		asm volatile("");
}

static char* outputCallback(const char* buf, void* a, int len)
{
	OBOS_UNUSED(a);
	for (size_t j = 0; j < nOutputCallbacks; j++)
		outputCallbacks[j].write(buf, len, outputCallbacks[j].userdata);
	return (char*)buf;
}
void OBOS_AddLogSource(const log_backend* backend)
{
	if (nOutputCallbacks++ < CALLBACK_COUNT)
		outputCallbacks[nOutputCallbacks - 1] = *backend;
}
OBOS_EXPORT size_t printf(const char* format, ...)
{
	va_list list;
	va_start(list, format);
	size_t ret = vprintf(format, list);
	va_end(list);
	return ret;
}
OBOS_EXPORT size_t vprintf(const char* format, va_list list)
{
	if (!s_printfLockInitialized)
	{
		s_printfLockInitialized = true;
		s_printfLock = Core_SpinlockCreate();
	}
	char ch[8];
	irql oldIrql = Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
	size_t ret = stbsp_vsprintfcb(outputCallback, nullptr, ch, format, list);
	Core_SpinlockRelease(&s_printfLock, oldIrql);
	return ret;
}
OBOS_EXPORT size_t snprintf(char* buf, size_t bufSize, const char* format, ...)
{
	va_list list;
	va_start(list, format);
	size_t ret = vsnprintf(buf, bufSize, format, list);
	va_end(list);
	return ret;
}
OBOS_EXPORT size_t vsnprintf(char* buf, size_t bufSize, const char* format, va_list list)
{
	return stbsp_vsnprintf(buf, bufSize, format, list);
}
OBOS_EXPORT size_t puts(const char *s)
{
	size_t len = strlen(s);
	irql oldIrql = Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
	outputCallback(s, nullptr, len);
	Core_SpinlockRelease(&s_printfLock, oldIrql);
	return len;
}

static void con_output(const char *str, size_t sz, void* userdata)
{
	if (userdata)
		for (size_t i = 0; i < sz; i++)
			OBOS_WriteCharacter((text_renderer_state*)userdata, str[i]);
}
static void con_set_color(color c, void* userdata)
{
	if (userdata)
	{
#define RGBX(r,g,b) (((uint32_t)(r) << 24) | ((uint32_t)(g) << 16) | ((uint32_t)(b) << 8))
		static color color_to_rgbx[16] = {
			RGBX(0x00,0x00,0x00), // COLOR_BLACK
			RGBX(0x00,0x00,0xff), // COLOR_BLUE
			RGBX(0x00,0x80,0x00), // COLOR_GREEN
			RGBX(0x00,0xff,0xff), // COLOR_CYAN
			RGBX(0xff,0x00,0x00), // COLOR_RED
			RGBX(0xff,0x00,0xff), // COLOR_MAGENTA
			RGBX(0x8b,0x45,0x13), // COLOR_BROWN
			RGBX(0xd3,0xd3,0xd3), // COLOR_LIGHT_GREY
			RGBX(0xa9,0xa9,0xa9), // COLOR_DARK_GREY
			RGBX(0x00,0xbf,0xff), // COLOR_LIGHT_BLUE
			RGBX(0x90,0xee,0x90), // COLOR_LIGHT_GREEN
			RGBX(0xe0,0xff,0xff), // COLOR_LIGHT_CYAN
			RGBX(0xf0,0x80,0x80), // COLOR_LIGHT_RED
			RGBX(0xff,0x80,0xff), // COLOR_LIGHT_MAGENTA
			RGBX(0xff,0xff,0x00), // COLOR_YELLOW
			RGBX(0xff,0xff,0xff), // COLOR_WHITE
		};
		uint32_t new_color = color_to_rgbx[c];
		text_renderer_state* state = userdata;
		state->fg_color = new_color;
	}
}
static void con_reset_color(void *userdata)
{
	con_set_color(COLOR_WHITE, userdata);
}
log_backend OBOS_ConsoleOutputCallback = {
	.write=con_output,
	.set_color=con_set_color,
	.reset_color=con_reset_color,
	.userdata=&OBOS_TextRendererState
};
