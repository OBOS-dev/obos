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
typedef _Bool bool;
#endif

#define OBOS_NORETURN [[_Noreturn]]
#define OBOS_NODISCARD [[__nodiscard__]]
#define OBOS_NODISCARD_REASON(why) [[__nodiscard__(why)]]