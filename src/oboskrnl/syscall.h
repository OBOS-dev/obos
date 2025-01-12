/*
 * oboskrnl/syscall.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <klog.h>
#include <error.h>

// All syscall number ranges outside this are reserved.
#define SYSCALL_BEGIN (0)
#define SYSCALL_END (0x10000)
#define ARCH_SYSCALL_BEGIN (0x80000000)
#define ARCH_SYSCALL_END (0x80010000)

#define IS_ARCH_SYSCALL(n) ((n) >= ARCH_SYSCALL_BEGIN)

extern OBOS_EXPORT uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN];
extern OBOS_EXPORT uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN];

typedef uint64_t syscall_ret_t;

// NOTE (for kernel devs): Syscalls can have a max of 5 parameters, any more paramters must be passed through a memory buffer.

// Note: entry can return a max of syscall_ret_t, and can take a max of 5 arguments.
inline static void OBOS_RegisterSyscall(uint32_t num, uintptr_t entry)
{
    if (IS_ARCH_SYSCALL(num))
        OBOS_ArchSyscallTable[num-ARCH_SYSCALL_BEGIN] = entry;
    else
        OBOS_SyscallTable[num-SYSCALL_BEGIN] = entry;
}
void OBOSS_InitializeSyscallInterface();

// if buf and sz_buf are nullptr, the function silently fails
// if ustr is nullptr, OBOS_STATUS_INVALID_ARGUMENT is returned
// if a page fault occurs reading the string, then OBOS_STATUS_PAGE_FAULT is returned
// if all goes well, you get a string and its size back, and OBOS_STATUS_SUCCESS
obos_status OBOSH_ReadUserString(const char* ustr, char* buf, size_t* sz_buf);
