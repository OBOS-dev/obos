/*
	oboskrnl/klog.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <klog.h>
#include <text.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

#include <scheduler/process.h>

#include <driver_interface/driverId.h>
#include <driver_interface/loader.h>

#include <irq/irql.h>

#include <uacpi_libc.h>

#include <stdarg.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#define STB_SPRINTF_NOFLOAT 1
#define STB_SPRINTF_IMPLEMENTATION 1
#define STB_SPRINTF_MIN 8
#include <external/stb_sprintf.h>
#pragma GCC diagnostic pop

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
static void common_log(log_level minimumLevel, const char* log_prefix, const char* format, va_list list)
{
	if (!s_loggerLockInitialized)
	{
		s_loggerLock = Core_SpinlockCreate();
		s_loggerLockInitialized = true;
	}
	if (s_logLevel > minimumLevel)
		return;
	uint8_t oldIrql = Core_SpinlockAcquire(&s_loggerLock);
	printf("[ %s ] ", log_prefix);
	vprintf(format, list);
	Core_SpinlockRelease(&s_loggerLock, oldIrql);
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
        "  | |/ /(_))   ((_p) _(_/( (_)) | |  ((_)_\\ ((_)_  _(_/( (_) ((_)\r\n"
        "  | ' < / -_) | '_|| ' \\))/ -_)| |  | '_ \\)/ _` || ' \\))| |/ _|\r\n"
        "  |_|\\_\\\\___| |_|  |_||_| \\___||_|  | .__/ \\__,_||_||_| |_|\\__|\r\n"
        "                                    |_|\r\n";
#ifndef OBOS_UP
	if (OBOSS_HaltCPUs)
		OBOSS_HaltCPUs();
#endif
	Core_SpinlockForcedRelease(&s_printfLock);
	Core_SpinlockForcedRelease(&s_loggerLock);
	OBOS_TextRendererState.fb.backbuffer_base = nullptr; // the back buffer might cause some trouble.
	uint8_t oldIrql = Core_RaiseIrqlNoThread(IRQL_MASKED);
	OBOS_UNUSED(oldIrql);
	printf("\n%s\n", ascii_art);
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
			printf("%p        ", (void*)pc);
			if (sym)
			{
				if (drv)
					printf("%*s!%s+%x", uacpi_strnlen(drv->header.driverName, 64), drv->header.driverName, sym->name, pc-sym->address);
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
// 	printf("\n\tAddress");
// #if UINTPTR_MAX == UINT64_MAX
// 	// We want 8+9 bytes of padding
// 	printf("                  ");
// #elif UINTPTR_MAX == UINT32_MAX
// 	// We want 8+2 bytes of padding
// 	printf("          ");
// #endif
// 	printf("Main TID");
// 	// We want 4+1 bytes of padding
// 	printf("     ");
// 	printf("Driver Name\n");
// 	for (driver_node *node = Drv_LoadedDrivers.head; node; )
// 	{
// 		if (!node->data)
// 			goto next;
// 		printf("\t%p     ", node->data->base);
// 		printf("%12ld     ", node->data->main_thread ? node->data->main_thread->tid : -1);
// 		if (uacpi_strnlen(node->data->header.driverName, 64))
// 			printf("%*s\n", uacpi_strnlen(node->data->header.driverName, 64), node->data->header.driverName);
// 		else
// 		 	printf("Unknown\n");
// 		next:
// 		node = node->next;
// 	}
	while (1)
		asm volatile("");
}


static char* outputCallback(const char* buf, void* a, int len)
{
	OBOS_UNUSED(a);
	for (size_t i = 0; i < (size_t)len; i++)
	{
		OBOS_WriteCharacter(&OBOS_TextRendererState, buf[i]);
#ifdef __x86_64__
		outb(0xE9, buf[i]);
#elif defined(__m68k__)
	extern uintptr_t Arch_TTYBase;
	if (Arch_TTYBase)
	    ((uint32_t*)Arch_TTYBase)[0] = buf[i]; // Enable device through CMD register
#endif
	}
	return (char*)buf;
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
	uint8_t oldIrql = Core_SpinlockAcquireExplicit(&s_printfLock, IRQL_DISPATCH, true);
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