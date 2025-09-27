/*
 * oboskrnl/utils/string.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <utils/string.h>

#include <allocators/base.h>

void OBOS_StringSetAllocator(string* obj, allocator_info* allocator)
{
    obj->allocator = allocator;
}
void OBOS_InitString(string* obj, const char* str)
{
    OBOS_InitStringLen(obj, str, strlen(str));
}
void OBOS_InitStringLen(string* obj, const char* str, size_t len)
{
    if (!obj->allocator)
        obj->allocator = OBOS_KernelAllocator;
    obj->ls = nullptr;
    obj->cap = 0;
    obj->len = 0;
    if (len <= 32)
    {
        memzero(obj->sso, 33);
        memcpy(obj->sso, str, len);
        obj->cap = 32;
        obj->len = len;
    }
    else 
    {
        OBOS_SetCapacityString(obj, len);
        obj->len = len;
        memzero(obj->ls, obj->cap);
        memcpy(obj->ls, str, len);
    }
}
void OBOS_AppendStringC(string* obj, const char* str)
{
    size_t str_len = strlen(str);
    size_t newlen = obj->len + str_len;
    OBOS_ResizeString(obj, newlen);
    memcpy(OBOS_GetStringPtr(obj) + (newlen-str_len), str, str_len);
}
void OBOS_AppendStringS(string* obj, string* str)
{
    size_t newlen = obj->len + str->len;
    OBOS_ResizeString(obj, newlen);
    memcpy(OBOS_GetStringPtr(obj) + (newlen-str->len), OBOS_GetStringPtr(str), str->len);
}
void OBOS_ResizeString(string* obj, size_t len)
{
    if (((len + 0x1f) & ~0x1f) != obj->cap)
        OBOS_SetCapacityString(obj, len);
    if (len < obj->len)
        memzero(OBOS_GetStringPtr(obj) + obj->len, obj->len - len);
    else
        memzero(OBOS_GetStringPtr(obj) + obj->len, len - obj->len);
    obj->len = len;
    OBOS_GetStringPtr(obj)[obj->len] = 0;
}
void OBOS_SetCapacityString(string* obj, size_t cap)
{
    if (cap <= 32)
        return;
    cap = (cap + 0x1f) & ~0x1f;
    size_t oldCap = obj->cap;
    obj->cap = cap;
    obj->ls = obj->allocator->Reallocate(obj->allocator, obj->ls, obj->cap, oldCap, nullptr);
    if (oldCap <= 32)
        memcpy(obj->ls, obj->sso, oldCap);
    memzero(obj->ls + oldCap, cap-oldCap);
}
size_t OBOS_GetStringCapacity(const string* obj)
{
    return obj ? obj->cap : 0;
}
size_t OBOS_GetStringSize(const string* obj)
{
    return obj ? obj->len : 0;
}
char* OBOS_GetStringPtr(string* obj)
{
    return obj->cap <= 32 ? obj->sso : obj->ls;
}
const char* OBOS_GetStringCPtr(const string* obj)
{
    return obj->cap <= 32 ? obj->sso : obj->ls;
}
bool OBOS_CompareStringS(const string* str1, const string* str2)
{
    return OBOS_CompareStringNC(str1, OBOS_GetStringCPtr(str2), str2->len);
}
bool OBOS_CompareStringC(const string* str1, const char* str2)
{
    return OBOS_CompareStringNC(str1, str2, strlen(str2));
}
bool OBOS_CompareStringNC(const string* str1, const char* str2, size_t str2len)
{
    if (str2len != str1->len)
        return false;
    return memcmp(OBOS_GetStringCPtr(str1), str2, str2len);
}
void OBOS_FreeString(string* obj)
{
    if (obj->cap <= 32)
        return;
    obj->allocator->Free(obj->allocator, obj->ls, obj->cap);
}
