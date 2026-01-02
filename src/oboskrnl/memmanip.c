/*
    oboskrnl/memmanip.c

    Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <mm/context.h>
#include <mm/bare_map.h>
#include <mm/alloc.h>
#include <mm/pmm.h>
#include <mm/page.h>
#include <mm/handler.h>

#if !OBOS_ARCH_HAS_USR_MEMCPY
obos_status memcpy_usr_to_k(void* k_dest, const void* usr_src, size_t count)
{
    if (CoreS_GetCPULocalPtr()->currentContext == &Mm_KernelContext)
        return memcpy(k_dest, usr_src, count) ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
    obos_status status = OBOS_STATUS_SUCCESS;
    const void* ubuf = Mm_MapViewOfUserMemory(ctx, (void*)usr_src, nullptr, count, OBOS_PROTECTION_READ_ONLY, true, &status);
    if (obos_is_error(status))
        return status;
    memcpy(k_dest, ubuf, count);
    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)ubuf, count);
    return OBOS_STATUS_SUCCESS;
}
obos_status memcpy_k_to_usr(void* usr_dest, const void* k_src, size_t count)
{
    if (CoreS_GetCPULocalPtr()->currentContext == &Mm_KernelContext)
        return memcpy(usr_dest, k_src, count) ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
    obos_status status = OBOS_STATUS_SUCCESS;
    void* ubuf = Mm_MapViewOfUserMemory(ctx, usr_dest, nullptr, count, 0, true, &status);
    if (obos_is_error(status))
        return status;
// #if OBOS_DEBUG && !OBOS_KASAN_ENABLED
//     for (size_t i = 0; i < count; i++)
//         OBOS_ENSURE((((unsigned char*)k_src)[i]) != 0xde);
// #endif
    memcpy(ubuf, k_src, count);
    Mm_VirtualMemoryFree(&Mm_KernelContext, ubuf, count);
    return OBOS_STATUS_SUCCESS;
}
#endif

#if !OBOS_ARCH_HAS_MEMSET
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memset(void* blk, int val, size_t count)
{
    char* buf = (char*)blk;
    for (size_t i = 0; i < count; i++)
        buf[i] = val;
    return blk;
}
#endif

#if !OBOS_ARCH_HAS_MEMZERO
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memzero(void* blk, size_t count)
{
    return memset(blk, 0, count);
}
#endif

#if !OBOS_ARCH_HAS_MEMCPY
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memcpy(void* blk1_, const void* blk2_, size_t count)
{
    for (size_t i = 0; i < count; i++)
        ((uint8_t*)blk1_)[i] = ((uint8_t*)blk2_)[i];
    return blk1_;
}
#endif

#if !OBOS_ARCH_HAS_MEMCMP
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp(const void* blk1_, const void* blk2_, size_t count)
{
    const uint8_t *blk1 = (const uint8_t*)blk1_;
    const uint8_t *blk2 = (const uint8_t*)blk2_;
    for (size_t i = 0; i < count; i++)
        if (blk1[i] != blk2[i])
            return false;
    return true;
}
#endif

#if !OBOS_ARCH_HAS_MEMCMP_B
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp_b(const void* blk1_, int against, size_t count)
{
    const uint8_t *blk1 = (const uint8_t*)blk1_;
    for (size_t i = 0; i < count; i++)
        if (blk1[i] != (uint8_t)against)
            return false;
    return true;
}
#endif

#if !OBOS_ARCH_HAS_STRCMP
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool strcmp(const char* str1, const char* str2)
{
    size_t sz1 = strlen(str1);
    size_t sz2 = strlen(str2);
    if (sz1 != sz2)
        return false;
    return memcmp(str1, str2, sz1);
}
#endif

#if !OBOS_ARCH_HAS_STRNCMP
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool strncmp(const char* str1, const char* str2, size_t len)
{
    size_t sz1 = strnlen(str1, len);
    size_t sz2 = strnlen(str2, len);
    if (sz1 != sz2)
        return false;
    return memcmp(str1, str2, sz1);
}
#endif

#if !OBOS_ARCH_HAS_STRLEN
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strlen(const char* str)
{
    size_t i = 0;
    for (; str[i]; i++)
        ;
    return i;
}
#endif

#if !OBOS_ARCH_HAS_STRNLEN
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strnlen(const char* str, size_t maxcnt)
{
    if (!str)
        return 0;
    size_t i = 0;
    for (; i < maxcnt && str[i]; i++);
    return i;
}
#endif

#if !OBOS_ARCH_HAS_STRCHR
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strchr(const char* str, char ch)
{
    size_t i = 0;
    for (; str[i] != ch && str[i]; i++)
        ;
    return i + (str[i] == ch ? 1 : 0);
}
#endif

#if !OBOS_ARCH_HAS_STRNCHR
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strnchr(const char* str, char ch, size_t count)
{
    size_t i = 0;
    for (; i < count; i++)
        if (str[i] == ch)
            break;
    if (i == count)
        return count;
    return i + (str[i] == ch ? 1 : 0);
}
#endif

OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN int memcmp_std(const void* blk1_, const void* blk2_, size_t count)
{
    const uint8_t *blk1 = (const uint8_t*)blk1_;
    const uint8_t *blk2 = (const uint8_t*)blk2_;
    for (size_t i = 0; i < count; i++)
    {
        if (blk1[i] < blk2[i])
            return -1;
        else if (blk1[i] > blk2[i])
            return 1;
        else
            continue;
    } 
    return 0;
}

OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN int strcmp_std(const char* src1, const char* src2)
{
    size_t len1 = strlen(src1);
    size_t len2 = strlen(src2);
    if (len1 < len2)
        return -1;
    else if (len1 > len2)
        return 1;
    return memcmp_std(src1, src2, len1);
}