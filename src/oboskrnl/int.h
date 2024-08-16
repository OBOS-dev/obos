/*
	oboskrnl/int.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <stdint.h>
#include <stddef.h>

#define obos_expect(expr, eval) __builtin_expect((expr), (eval))

#if OBOS_KERNEL
#	define OBOS_EXPORT __attribute__((visibility("default")))
// Usually redundant.
#	define OBOS_LOCAL __attribute__((visibility("hidden")))

#	define DRV_EXPORT __attribute__((visibility("default")))
// Usually redundant.
#	define DRV_LOCAL __attribute__((visibility("hidden")))
#elif defined(OBOS_DRIVER)
#	define OBOS_EXPORT 
// Usually redundant.
#	define OBOS_LOCAL

#	define DRV_EXPORT __attribute__((visibility("default")))
// Usually redundant.
#	define DRV_LOCAL __attribute__((visibility("hidden")))
#else
#	error Only the kernel and drivers can access kernel headers.
#endif

#define OBOS_WEAK __attribute__((weak))

#if UINTPTR_MAX == UINT64_MAX
#define PTR_BITS 64
#elif UINTPTR_MAX == UINT32_MAX
#define PTR_BITS 32
#elif UINTPTR_MAX == UINT16_MAX
#define PTR_BITS 16
#endif

typedef uint32_t uid, gid;
#define ROOT_UID 0
#define ROOT_GID 0

#if !defined(__cplusplus) && !defined(true) && !defined(false)
#	define true (1)
#	define false (0)
typedef _Bool bool;
#endif
#ifndef __cplusplus
#	define nullptr ((void*)0)
#	ifndef IS_UACPI_BUILD
#		undef NULL
#	endif
// Do all this to make sure intellisense is happy.
#	if __STDC_VERSION__ >= 201112L && __STDC_VERSION__ < 202311L
#		define OBOS_ALIGNAS(x) _Alignas(x)
#	elif __STDC_VERSION__ < 201112L
#		ifdef __GNUC__
#			define OBOS_ALIGNAS(x) __attribute__((alignas(x))) 
#		elif defined(_MSC_VER)
#			define OBOS_ALIGNAS(x) __declspec(align(x))
#		endif
#	else
#		define OBOS_ALIGNAS(x) alignas(x)
#	endif
#else
#	define OBOS_ALIGNAS(x) alignas(x)
#	ifndef IS_UACPI_BUILD
#		undef NULL
#	endif
#endif

#define OBOS_NORETURN [[noreturn]]
#define OBOS_UNUSED(x) (void)(x)
#ifdef __GNUC__
#	define OBOS_NO_KASAN __attribute__((no_sanitize("address")))
#	define OBOS_NO_UBSAN __attribute__((no_sanitize("undefined")))
#	define OBOS_NODISCARD __attribute__ ((warn_unused_result))
#	define OBOS_NODISCARD_REASON(why) 
#elif _MSC_VER
#	define OBOS_NO_KASAN
#	define OBOS_NO_UBSAN
#else
#	define OBOS_NO_KASAN
#	define OBOS_NO_UBSAN
#endif
#if __STDC_VERSION__ < 202311L && __STDC_VERSION__ >= 201112L
#	define OBOS_STATIC_ASSERT(expr, msg) _Static_assert(expr, msg)
#elif __STDC_VERSION__ == 202311L
#	define OBOS_STATIC_ASSERT(expr, msg) static_assert(expr, msg)
#else
#	define OBOS_STATIC_ASSERT(expr, msg) 
#endif

#define BIT_TYPE(b, type) (1##type << (b))
#define BIT(b) BIT_TYPE(b, U)
#if __STDC_NO_ATOMICS__
#	error No atomics supported by the compiler.
#endif
#if __STDC_HOSTED__
#	error Must be compiled as freestanding.
#endif
#define OBOS_PAGEABLE_VARIABLE __attribute__((section(".pageable.data")))
#define OBOS_PAGEABLE_RO_VARIABLE __attribute__((section(".pageable.rodata")))
#define OBOS_PAGEABLE_FUNCTION __attribute__((section(".pageable.text")))