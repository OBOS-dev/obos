#pragma once

#include <int.h>

#ifndef UACPI_ARCH_FLUSH_CPU_CACHE
#define UACPI_ARCH_FLUSH_CPU_CACHE() asm volatile ("wbinvd")
#endif

typedef uint8_t uacpi_cpu_flags;