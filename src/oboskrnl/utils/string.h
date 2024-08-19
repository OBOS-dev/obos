/*
 * oboskrnl/utils/string.h
 *
 * Copyright (c) 2024 Omar Berrow
 *
 * String manipulation routines.
*/

#pragma once

#include <int.h>

#include <allocators/base.h>

typedef struct string
{
    // small string optimization.
    // this buffer is used when cap < 32
    char sso[33];
    char *ls;
    size_t len, cap;
    allocator_info* allocator;
} string;

void OBOS_StringSetAllocator(string* obj, allocator_info* allocator);
void OBOS_InitString(string* obj, const char* str);
void OBOS_InitStringLen(string* obj, const char* str, size_t len);
void OBOS_AppendStringC(string* obj, const char* str);
void OBOS_AppendStringS(string* obj, string* str);
void OBOS_ResizeString(string* obj, size_t len);
void OBOS_SetCapacityString(string* obj, size_t cap);
size_t OBOS_GetStringCapacity(const string* obj);
size_t OBOS_GetStringSize(const string* obj);
char* OBOS_GetStringPtr(string* obj);
const char* OBOS_GetStringCPtr(const string* obj);
void OBOS_FreeString(string* obj);
bool OBOS_CompareStringS(const string* str1, const string* str2);
bool OBOS_CompareStringC(const string* str1, const char* str2);
bool OBOS_CompareStringNC(const string* str1, const char* str2, size_t str2len);