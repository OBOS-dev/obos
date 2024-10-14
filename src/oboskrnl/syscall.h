/*
 * oboskrnl/syscall.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <klog.h>

// All syscall number ranges outside this are reserved.
#define SYSCALL_BEGIN (0)
#define SYSCALL_END (0x10000)
#define ARCH_SYSCALL_BEGIN (0x80000000)
#define ARCH_SYSCALL_END (0x80010000)

#define IS_ARCH_SYSCALL(n) ((n) >= ARCH_SYSCALL_BEGIN)

extern OBOS_EXPORT uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN];
extern OBOS_EXPORT uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN];

#if OBOS_ARCHITECTURE_BITS == 64
typedef __uint128_t syscall_ret_t;
#elif OBOS_ARCHITECTURE_BITS == 32
typedef uint64_t syscall_ret_t;
#else
#error Unknown.
#endif

// Note: entry can return a max of syscall_ret_t, and can take a max of 6 arguments.
inline static void OBOS_RegisterSyscall(uint32_t num, uintptr_t entry)
{
    if (IS_ARCH_SYSCALL(num))
        OBOS_ArchSyscallTable[num-ARCH_SYSCALL_BEGIN] = entry;
    else
        OBOS_ArchSyscallTable[num-SYSCALL_BEGIN] = entry;
}
void OBOSS_InitializeSyscallInterface();
