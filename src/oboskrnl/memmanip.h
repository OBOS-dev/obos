/*
	oboskrnl/memmanip.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

OBOS_EXPORT void*   memset(void* blk, int val, size_t count);
OBOS_EXPORT void*   memzero(void* blk, size_t count);
OBOS_EXPORT void*   memcpy(void* blk1, const void* blk2, size_t count);
OBOS_EXPORT bool    memcmp(const void* blk1, const void* blk2, size_t count);
OBOS_EXPORT int memcmp_std(const void* blk1, const void* blk2, size_t count);
OBOS_EXPORT bool  memcmp_b(const void* blk1, int against, size_t count);
OBOS_EXPORT bool    strcmp(const char* str1, const char* str2);
OBOS_EXPORT int strcmp_std(const char* str1, const char* str2);
OBOS_EXPORT bool   strncmp(const char* str1, const char* str2, size_t len);
OBOS_EXPORT size_t  strlen(const char* str);
OBOS_EXPORT size_t strnlen(const char* str, size_t maxcnt);
OBOS_EXPORT size_t  strchr(const char* str, char ch);
OBOS_EXPORT size_t strnchr(const char* str, char ch, size_t sz);

obos_status memcpy_usr_to_k(void* k_dest, const void* usr_src, size_t count);
obos_status memcpy_k_to_usr(void* usr_dest, const void* k_src, size_t count);
