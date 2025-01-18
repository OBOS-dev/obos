/*
	oboskrnl/int.h

	Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <stdint.h>
#include <stddef.h>

#define obos_expect(expr, eval) __builtin_expect((expr), (eval))

#if defined(OBOS_KERNEL) || defined(OBOS_DRIVER)
#	define OBOS_EXPORT __attribute__((visibility("default")))
// Usually redundant.
#	define OBOS_LOCAL __attribute__((visibility("hidden")))

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
// #	define true (1)
// #	define false (0)
enum {false=0,true=1};
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
#	define OBOS_NODISCARD_REASON(why)  __attribute__ ((warn_unused_result))
#	define OBOS_MIN(val, val2) \
	({  typeof (val) _a = (val); \
		typeof (val2) _b = (val2); \
		_a < _b ? _a : _b; })
#	define OBOS_MAX(val, val2) \
	({  typeof (val) _a = (val); \
		typeof (val2) _b = (val2); \
		_a > _b ? _a : _b; })
#else
#	define OBOS_NO_KASAN
#	define OBOS_NO_UBSAN
#	define OBOS_NODISCARD
#	define OBOS_NODISCARD_REASON(why)
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
#if OBOS_KERNEL
#	define OBOS_PAGEABLE_VARIABLE __attribute__((section(".pageable.data")))
#	define OBOS_PAGEABLE_RO_VARIABLE __attribute__((section(".pageable.rodata")))
#	define OBOS_PAGEABLE_FUNCTION __attribute__((section(".pageable.text")))
#elif defined(OBOS_DRIVER) && !defined(__m68k__)
#	define OBOS_PAGEABLE_VARIABLE __attribute__((section(".pageable.data")))
#	define OBOS_PAGEABLE_RO_VARIABLE __attribute__((section(".pageable.rodata")))
#	define OBOS_PAGEABLE_FUNCTION __attribute__((section(".pageable.text")))
#else
#	define OBOS_PAGEABLE_VARIABLE
#	define OBOS_PAGEABLE_RO_VARIABLE
#	define OBOS_PAGEABLE_FUNCTION
#endif

#include <inc/dev_prefix.h>

#if defined(__x86_64__) || defined(__i386__)
#	define OBOSS_SpinlockHint() asm("pause")
#elif defined(__m68k__)
#	define OBOSS_SpinlockHint() asm("nop")
#else
#	define OBOSS_SpinlockHint() asm("")
#endif

#if defined(__x86_64__)
#   define host_to_be8(val)  __builtin_bswap8(val)
#   define host_to_be16(val) __builtin_bswap16(val)
#   define host_to_be32(val) __builtin_bswap32(val)
#   define host_to_be64(val) __builtin_bswap64(val)
#   define be8_to_host(val)  __builtin_bswap8(val)
#   define be16_to_host(val) __builtin_bswap16(val)
#   define be32_to_host(val) __builtin_bswap32(val)
#   define be64_to_host(val) __builtin_bswap64(val)
#   define host_to_le8(val)  (uint8_t)(val)
#   define host_to_le16(val) (uint16_t)(val)
#   define host_to_le32(val) (uint32_t)(val)
#   define host_to_le64(val) (uint32_t)(val)
#   define le8_to_host(val)  (uint8_t)(val)
#   define le16_to_host(val) (uint16_t)(val)
#   define le32_to_host(val) (uint32_t)(val)
#   define le64_to_host(val) (uint64_t)(val)
#elif defined(__m68k__)
#   define host_to_be8(val)  (uint8_t)(val)
#   define host_to_be16(val) (uint16_t)(val)
#   define host_to_be32(val) (uint32_t)(val)
#   define host_to_be64(val) (uint32_t)(val)
#   define be8_to_host(val)  (uint8_t)(val)
#   define be16_to_host(val) (uint16_t)(val)
#   define be32_to_host(val) (uint32_t)(val)
#   define be64_to_host(val) (uint64_t)(val)
#   define host_to_le8(val)  __builtin_bswap8(val)
#   define host_to_le16(val) __builtin_bswap16(val)
#   define host_to_le32(val) __builtin_bswap32(val)
#   define host_to_le64(val) __builtin_bswap64(val)
#   define le8_to_host(val)  __builtin_bswap8(val)
#   define le16_to_host(val) __builtin_bswap16(val)
#   define le32_to_host(val) __builtin_bswap32(val)
#   define le64_to_host(val) __builtin_bswap64(val)
#else
#   error Define required macros.
#endif