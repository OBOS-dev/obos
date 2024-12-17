/*
    oboskrnl/memmanip.c

    Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <mm/context.h>
#include <mm/bare_map.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

obos_status memcpy_usr_to_k(void* k_dest, const void* usr_src, size_t count)
{
    if (CoreS_GetCPULocalPtr()->currentContext == &Mm_KernelContext)
        return memcpy(k_dest, usr_src, count) ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
    // TODO: F****** implement virtual page locking
    // Mm_VirtualMemoryLock(ctx, futex, sizeof(*futex));
    uintptr_t usrphys = 0;
    size_t usroffset = (uintptr_t)usr_src % OBOS_PAGE_SIZE;
    long bytes_left = count;
    for (size_t i = 0; i < count; )
    {
        MmS_QueryPageInfo(ctx->pt, (uintptr_t)usr_src + i, nullptr, &usrphys);
        if (!usrphys)
        {
            // Mm_VirtualMemoryUnlock(ctx, futex, sizeof(*futex));
            return OBOS_STATUS_PAGE_FAULT;
        }
        size_t currCount = bytes_left > OBOS_PAGE_SIZE ? OBOS_PAGE_SIZE-((usrphys+usroffset)%OBOS_PAGE_SIZE) : (size_t)bytes_left;
        memcpy((void*)((uintptr_t)k_dest + i), MmS_MapVirtFromPhys(usrphys+usroffset), currCount);
        usroffset = 0;
        i += currCount;
        bytes_left -= currCount;
    }
    // Mm_VirtualMemoryUnlock(ctx, futex, sizeof(*futex));
    return OBOS_STATUS_SUCCESS;
}
obos_status memcpy_k_to_usr(void* usr_dest, const void* k_src, size_t count)
{
    if (CoreS_GetCPULocalPtr()->currentContext == &Mm_KernelContext)
        return memcpy(usr_dest, k_src, count) ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
    context* ctx = CoreS_GetCPULocalPtr()->currentContext;
    // TODO: F****** implement virtual page locking
    // Mm_VirtualMemoryLock(ctx, futex, sizeof(*futex));
    uintptr_t usrphys = 0;
    size_t usroffset = (uintptr_t)usr_dest % OBOS_PAGE_SIZE;
    long bytes_left = count;
    for (size_t i = 0; i < count; )
    {
        MmS_QueryPageInfo(ctx->pt, (uintptr_t)usr_dest + i, nullptr, &usrphys);
        if (!usrphys)
        {
            // Mm_VirtualMemoryUnlock(ctx, futex, sizeof(*futex));
            return OBOS_STATUS_PAGE_FAULT;
        }
        size_t currCount = bytes_left > OBOS_PAGE_SIZE ? OBOS_PAGE_SIZE-((usrphys+usroffset)%OBOS_PAGE_SIZE) : (size_t)bytes_left;
        memcpy(MmS_MapVirtFromPhys(usrphys+usroffset), (void*)((uintptr_t)k_src + i), currCount);
        usroffset = 0;
        i += currCount;
        bytes_left -= currCount;
    }
    // Mm_VirtualMemoryUnlock(ctx, futex, sizeof(*futex));
    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memset(void* blk, int val, size_t count)
{
    char* buf = (char*)blk;
    for (size_t i = 0; i < count; i++)
        buf[i] = val;
    return blk;
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memzero(void* blk, size_t count)
{
    return memset(blk, 0, count);
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN void* memcpy(void* blk1_, const void* blk2_, size_t count)
{
    char *blk1 = (char*)blk1_;
    const char *blk2 = (char*)blk2_;
    for (size_t i = 0; i < count; i++)
        blk1[i] = blk2[i];
    return blk1_;
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp(const void* blk1_, const void* blk2_, size_t count)
{
    const char *blk1 = (const char*)blk1_;
    const char *blk2 = (const char*)blk2_;
    for (size_t i = 0; i < count; i++)
        if (blk1[i] != blk2[i])
            return false;
    return true;
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool memcmp_b(const void* blk1_, int against, size_t count)
{
    const uint8_t *blk1 = (const uint8_t*)blk1_;
    for (size_t i = 0; i < count; i++)
        if (blk1[i] != (uint8_t)against)
            return false;
    return true;
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN bool strcmp(const char* str1, const char* str2)
{
    size_t sz1 = strlen(str1);
    size_t sz2 = strlen(str2);
    if (sz1 != sz2)
        return false;
    return memcmp(str1, str2, sz1);
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strlen(const char* str)
{
    size_t i = 0;
    for (; str[i]; i++)
        ;
    return i;
}
OBOS_WEAK OBOS_NO_KASAN OBOS_NO_UBSAN size_t strchr(const char* str, char ch)
{
    size_t i = 0;
    for (; str[i] != ch && str[i]; i++)
        ;
    return i + (str[i] == ch ? 1 : 0);
}
