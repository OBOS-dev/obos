/*
	oboskrnl/int.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <stdint.h>
#include <stddef.h>
//#include <stdbool.h>

#ifndef __cplusplus
#	define nullptr ((void*)0)
#	define true (1)
#	define false (0)
#	undef NULL
typedef _Bool bool;
#endif