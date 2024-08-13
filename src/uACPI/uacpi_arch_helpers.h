#pragma once

#include <int.h>

#if OBOS_ARCHITECTURE_HAS_ACPI
#   ifdef __x86_64__
#       define UACPI_ARCH_FLUSH_CPU_CACHE() asm volatile ("wbinvd")
#       define UACPI_ARCH_DISABLE_INTERRUPTS() asm volatile ("cli")
#   else
#       error Invalid architecture.
#   endif
#   ifndef UACPI_ATOMIC_LOAD_THREAD_ID
#       define UACPI_ATOMIC_LOAD_THREAD_ID(ptr) ((uacpi_thread_id)uacpi_atomic_load_ptr(ptr))
#   endif

#   ifndef UACPI_ATOMIC_STORE_THREAD_ID
#       define UACPI_ATOMIC_STORE_THREAD_ID(ptr, value) uacpi_atomic_store_ptr(ptr, value)
#   endif
#endif

typedef uint8_t uacpi_cpu_flags;
typedef uint64_t uacpi_thread_id;

#ifndef UACPI_THREAD_ID_NONE
#    define UACPI_THREAD_ID_NONE ((uacpi_thread_id)-1)
#endif

/*
 * A sentinel value that the kernel promises to NEVER return from
 * uacpi_kernel_get_current_thread_id or this will break
 */