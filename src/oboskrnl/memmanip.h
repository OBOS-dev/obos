/*
	oboskrnl/memmanip.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

OBOS_EXPORT void*  memset(void* blk, uint32_t val, size_t count);
OBOS_EXPORT void*  memzero(void* blk, size_t count);
OBOS_EXPORT void*  memcpy(void* blk1, const void* blk2, size_t count);
OBOS_EXPORT bool   memcmp(const void* blk1, const void* blk2, size_t count);
OBOS_EXPORT bool   memcmp_b(const void* blk1, int against, size_t count);
OBOS_EXPORT bool   strcmp(const char* str1, const char* str2);
OBOS_EXPORT size_t strlen(const char* str);
OBOS_EXPORT size_t strchr(const char* str, char ch);