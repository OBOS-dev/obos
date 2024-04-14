#pragma once

#include <int.h>

#ifdef __x86_64__
#define UACPI_ARCH_FLUSH_CPU_CACHE() asm volatile ("wbinvd")
#define UACPI_ARCH_DISABLE_INTERRUPTS() asm volatile ("cli")
#else
#error Invalid architecture.
#endif

typedef uint8_t uacpi_cpu_flags;