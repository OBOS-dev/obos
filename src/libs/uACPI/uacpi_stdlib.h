/*
	libs/uACPI/uacpi_stdlib.h
	
	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#define UACPI_PRIx64 "lx"
#define UACPI_PRIX64 "lX"
#define UACPI_PRIu64 "lu"

#define PRIx64 UACPI_PRIx64
#define PRIX64 UACPI_PRIX64
#define PRIu64 UACPI_PRIu64

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

#define uacpi_offsetof(t, m) ((uintptr_t)(&((t*)0)->m))