/*
	oboskrnl/fb.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	void*  memset(void* blk, uint32_t val, size_t count);
	void*  memzero(void* blk, size_t count);
	void*  memcpy(void* blk1, const void* blk2, size_t count);
	bool   memcmp(const void* blk1, const void* blk2, size_t count);
	bool   memcmp(const void* blk1, int against, size_t count);
	bool   strcmp(const char* str1, const char* str2);
	size_t strlen(const char* str);
}