/*
	libs/uACPI/uacpi_stdlib.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

// Now handled somewhere else
// #define UACPI_PRIx64 "lx"
// #define UACPI_PRIX64 "lX"
// #define UACPI_PRIu64 "lu"

// #define PRIx64 UACPI_PRIx64
// #define PRIX64 UACPI_PRIX64
// #define PRIu64 UACPI_PRIu64

// #define uacpi_offsetof(t, m) ((uintptr_t)(&((t*)0)->m))

#define NULL ((void*)0)

#ifdef __cplusplus
extern "C" {
#endif
	void  *uacpi_memcpy(void *dest, const void* src, size_t sz);
	void  *uacpi_memset(void *dest, int src, size_t cnt);
	int    uacpi_memcmp(const void *src1, const void *src2, size_t cnt);
	int    uacpi_strncmp(const char *src1, const char *src2, size_t maxcnt);
	int    uacpi_strcmp(const char *src1, const char *src2);
	void  *uacpi_memmove(void *dest, const void* src, size_t sz);
	size_t uacpi_strnlen(const char *src, size_t maxcnt);
	size_t uacpi_strlen(const char *src);
	int    uacpi_snprintf(char* dest, size_t n, const char* format, ...);
#ifdef __cplusplus
}
#endif
#define uacpi_memcpy uacpi_memcpy
#define uacpi_memset uacpi_memset
#define uacpi_memmove uacpi_memmove
#define uacpi_memcmp uacpi_memcmp
#define uacpi_strcmp uacpi_strcmp
#define uacpi_strncmp uacpi_strncmp
#define uacpi_strnlen uacpi_strnlen
#define uacpi_strlen uacpi_strlen
#define uacpi_snprintf uacpi_snprintf