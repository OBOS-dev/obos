/*
 * oboskrnl/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <syscall.h>

#include <scheduler/schedule.h>
#include <scheduler/thread.h>

uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN] = {
    (uintptr_t)Core_ExitCurrentThread,
    (uintptr_t)Core_Yield,
};
uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN];

