/*
 * oboskrnl/utils/string.h
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * String manipulation routines.
*/

#pragma once

#include <int.h>

typedef struct string
{
    // small string optimization.
    // this buffer is used when cap < 32
    char sso[33];
    char *ls;
    size_t len, cap;
    struct allocator_info* allocator;
} string;

OBOS_EXPORT void OBOS_StringSetAllocator(string* obj, struct allocator_info* allocator);
OBOS_EXPORT void OBOS_InitString(string* obj, const char* str);
OBOS_EXPORT void OBOS_InitStringLen(string* obj, const char* str, size_t len);
OBOS_EXPORT void OBOS_AppendStringC(string* obj, const char* str);
OBOS_EXPORT void OBOS_AppendStringS(string* obj, string* str);
OBOS_EXPORT void OBOS_ResizeString(string* obj, size_t len);
OBOS_EXPORT void OBOS_SetCapacityString(string* obj, size_t cap);
OBOS_EXPORT size_t OBOS_GetStringCapacity(const string* obj);
OBOS_EXPORT size_t OBOS_GetStringSize(const string* obj);
OBOS_EXPORT char* OBOS_GetStringPtr(string* obj);
OBOS_EXPORT const char* OBOS_GetStringCPtr(const string* obj);
OBOS_EXPORT void OBOS_FreeString(string* obj);
OBOS_EXPORT bool OBOS_CompareStringS(const string* str1, const string* str2);
OBOS_EXPORT bool OBOS_CompareStringC(const string* str1, const char* str2);
OBOS_EXPORT bool OBOS_CompareStringNC(const string* str1, const char* str2, size_t str2len);