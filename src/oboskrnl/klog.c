/*
	oboskrnl/klog.c

	Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <cmdline.h>
#include <klog.h>
#include <stdarg.h>
#include <memmanip.h>
#include <text.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

#include <scheduler/process.h>

#include <driver_interface/driverId.h>
#include <driver_interface/loader.h>

#include <irq/irql.h>

// #define STB_SPRINTF_NOFLOAT 1
// #define STB_SPRINTF_IMPLEMENTATION 1
// #define STB_SPRINTF_MIN 8
// #include <external/stb_sprintf.h>

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS 0
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS 1
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS 0
#define NANOPRINTF_IMPLEMENTATION
#include <external/nanoprintf.h>

#ifdef __x86_64__
#	include <arch/x86_64/asm_helpers.h>
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
	if (level > LOG_LEVEL_NONE || level < 0)
		return;
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
	irql oldIrql = 0xfe;
	if (Core_GetIrql() > IRQL_DISPATCH)
		oldIrql = Core_SpinlockAcquired(&s_loggerLock) ? IRQL_INVALID : Core_SpinlockAcquireExplicit(&s_loggerLock, IRQL_DISPATCH, true);
	else
 		oldIrql = Core_SpinlockAcquireExplicit(&s_loggerLock, IRQL_DISPATCH, true);
	if (oldIrql == 0xfe)
		return;
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
OBOS_EXPORT void OBOS_LibCLog(const char* format, ...)
{
	if (!s_loggerLockInitialized)
	{
		s_loggerLock = Core_SpinlockCreate();
		s_loggerLockInitialized = true;
	}
	if (s_logLevel > LOG_LEVEL_LOG)
		return;
	static bool enable_libc_log = false, has_cached_result = false;
	if (!has_cached_result)
	{
		enable_libc_log = !OBOS_GetOPTF("disable-libc-log");
		has_cached_result = true;
	}
	if (!enable_libc_log)
		return;
	irql oldIrql = Core_SpinlockAcquire(&s_loggerLock);

	OBOS_SetColor(COLOR_GREEN);
	printf("[ LIBC  ] ");
	va_list list;
	va_start(list, format);
	vprintf(format, list);
	va_end(list);

	OBOS_ResetColor();
	Core_SpinlockRelease(&s_loggerLock, oldIrql);
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
static int panic_max_depth = 5; 
OBOS_NORETURN OBOS_NO_KASAN OBOS_EXPORT  __attribute__((no_stack_protector)) void OBOS_Panic(panic_reason reason, const char* format, ...)
{
	static const char ascii_art[] =
		"       )\r\n"
        "    ( /(                        (\r\n"
        "    )\\())  (   (             (  )\\             )        (\r\n"
        "   ((_)\\  ))\\  )(    (      ))\\((_)  `  )   ( /(   (    )\\   (\r\n"
        "  (_ ((_)/((_)(()\\   )\\ )  /((_)_    /(/(   )(_))  )\\ )((_)  )\\\r\n"
        "  | |/ /(_))   ((_) _(_/( (_)) | |  ((_)_\\ ((_)_  _(_/( (_) ((_)\r\n"
        "  | ' < / -_) | '_|| ' \\))/ -_)| |  | '_ \\)/ _` || ' \\))| |/ _|\r\n"
        "  |_|\\_\\\\___| |_|  |_||_| \\___||_|  | .__/ \\__,_||_||_| |_|\\__|\r\n"
        "                                    |_|\r\n";

	if (!(--panic_max_depth))
		while(1)
	        asm volatile("");
#ifndef OBOS_UP
	if (OBOSS_HaltCPUs)
		OBOSS_HaltCPUs();
#endif

#if OBOS_ENABLE_PROFILING
	prof_stop();
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
#ifdef __x86_64
	char cpu_vendor[13] = {0};
	memset(cpu_vendor, 0, 13);
	__cpuid__(0, 0, nullptr, (uint32_t*)&cpu_vendor[0],(uint32_t*)&cpu_vendor[8], (uint32_t*)&cpu_vendor[4]);
	uint32_t ecx = 0;
	__cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
	bool isHypervisor = ecx & BIT_TYPE(31, UL) /* Hypervisor bit: Always 0 on physical CPUs. */;
	char brand_string[49];
	memset(brand_string, 0, sizeof(brand_string));
	__cpuid__(0x80000002, 0, (uint32_t*)&brand_string[0], (uint32_t*)&brand_string[4], (uint32_t*)&brand_string[8], (uint32_t*)&brand_string[12]);
	__cpuid__(0x80000003, 0, (uint32_t*)&brand_string[16], (uint32_t*)&brand_string[20], (uint32_t*)&brand_string[24], (uint32_t*)&brand_string[28]);
	__cpuid__(0x80000004, 0, (uint32_t*)&brand_string[32], (uint32_t*)&brand_string[36], (uint32_t*)&brand_string[40], (uint32_t*)&brand_string[44]);
#elif defined(__m68k__)
	char cpu_vendor[13] = { "Motorola" };
	char brand_string[49] = { "Motorola 68040" };
	bool isHypervisor = false;
#endif
	printf("Kernel Panic in OBOS %s built on %s.\nCrash is on CPU %d in thread %d, owned by process %d. Reason: %s.\n", GIT_SHA1, __DATE__ " at " __TIME__, getCPUId(), getTID(), getPID(), OBOSH_PanicReasonToStr(reason));
	printf("Currently running on a %s. We are currently %srunning on a hypervisor\n", brand_string, isHypervisor ? "" : "not ");
	printf("Information on the crash is below:\n");
	va_list list;
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	if (OBOSS_StackFrameNext && OBOSS_StackFrameGetPC)
	{
		printf("\n\tAddress");
#if UINTPTR_MAX == UINT64_MAX
		// We want 9+8 bytes of padding
		printf("                 ");
#elif UINTPTR_MAX == UINT32_MAX
		// We want 1+8 bytes of padding
		printf("         ");
#endif
		printf("Symbol\n");
		stack_frame curr = OBOSS_StackFrameNext(nullptr);
		while (curr)
		{
			// Resolve the symbol from the address.
			uintptr_t pc = OBOSS_StackFrameGetPC(curr);
			driver_id* drv = nullptr;
			driver_symbol* sym = DrvH_ResolveSymbolReverse(pc, &drv);
			printf("%0*p        ", sizeof(uintptr_t)*2, (void*)pc);
			if (sym)
			{
				if (drv)
					printf("%*s!%s+%x", strnlen(drv->header.driverName, 64), drv->header.driverName, sym->name, pc-sym->address);
				else 
					printf("oboskrnl!%s+%x", sym->name, pc-sym->address);
			}
			else
				printf("%s", pc == 0 ? "End" : "Unresolved/External");
			printf("\n");
			curr = OBOSS_StackFrameNext(curr);
		}
	}
	// unsafe to do unfortunately
	// todo: make safer
	printf("\n\tAddress");
#if UINTPTR_MAX == UINT64_MAX
	// We want 8+9 bytes of padding
	printf("                  ");
#elif UINTPTR_MAX == UINT32_MAX
	// We want 8+2 bytes of padding
	printf("          ");
#endif
	printf("Main TID");
	// We want 4+1 bytes of padding
	printf("     ");
	printf("Driver Name\n");
	for (driver_node *node = Drv_LoadedDrivers.head; node; )
	{
		if (!node->data)
			goto next;
		printf("\t%p     ", node->data->base);
		printf("%12ld     ", node->data->main_thread ? node->data->main_thread->tid : -1);
		if (strnlen(node->data->header.driverName, 64))
			printf("%*s\n", strnlen(node->data->header.driverName, 64), node->data->header.driverName);
		else
		 	printf("Unknown\n");
		next:
		node = node->next;
	}
	while (1)
		asm volatile("");
}

static void outputCallback(int val, void* a)
{
	OBOS_UNUSED(a);
	// Cast val to a char for big endian systems.
	char c = val;
	for (size_t j = 0; j < nOutputCallbacks; j++)
		outputCallbacks[j].write(&c, 1, outputCallbacks[j].userdata);
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
	irql oldIrql = 0xfe;
	if (Core_GetIrql() > IRQL_DISPATCH)
		oldIrql = Core_SpinlockAcquired(&s_printfLock) ? IRQL_INVALID : Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
	else
	 	oldIrql = Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
	if (oldIrql == 0xfe)
		return 0;
	size_t ret = npf_vpprintf(outputCallback, nullptr, format, list);
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
	return npf_vsnprintf(buf, bufSize, format, list);
}
OBOS_EXPORT size_t puts(const char *s)
{
	irql oldIrql = Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
	size_t i = 0;
	for (; s[i]; i++)
		outputCallback(s[i], nullptr);
	Core_SpinlockRelease(&s_printfLock, oldIrql);
	return i;
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
