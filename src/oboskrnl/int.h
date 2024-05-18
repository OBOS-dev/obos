/*
	oboskrnl/int.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <error.h>
//#include <stdbool.h>

#ifndef __cplusplus
#	define nullptr ((void*)0)
#	define true (1)
#	define false (0)
#	undef NULL
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
typedef _Bool bool;
#else
#	define OBOS_ALIGNAS(x) alignas(x)
#	undef NULL
#endif

#define OBOS_NORETURN [[_Noreturn]]
#define OBOS_NODISCARD [[__nodiscard__]]
#define OBOS_NODISCARD_REASON(why) [[__nodiscard__(why)]]
#define OBOS_UNUSED(x) (void)(sizeof((x), 0))
#ifdef __GNUC__
#	define OBOS_NO_KASAN __attribute__((no_sanitize("address")))
#	define OBOS_NO_UBSAN __attribute__((no_sanitize("undefined")))
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
