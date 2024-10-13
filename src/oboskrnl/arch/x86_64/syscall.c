/*
 * oboskrnl/arch/x86_64/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include "memmanip.h"
#include <int.h>
#include <syscall.h>

#include <arch/x86_64/asm_helpers.h>

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083
#define IA32_FSTAR 0xC0000084

void test_syscall(const char* user_buf)
{
    char buf[0x100];
    for (size_t i = 0; buf[i] && i < 0x100; i++)
        memcpy_usr_to_k(buf+i, user_buf+i, 1);
    OBOS_Debug("hai\n");
    OBOS_Debug("usermode says '%s'\n", buf);
}
uintptr_t OBOS_SyscallTable[SYSCALL_END-SYSCALL_BEGIN] = {
    (uintptr_t)test_syscall
};
uintptr_t OBOS_ArchSyscallTable[ARCH_SYSCALL_END-ARCH_SYSCALL_BEGIN];

extern void Arch_SyscallTrapHandler();
void OBOSS_InitializeSyscallInterface()
{
    // Enable IA32_EFER.SCE
    // This is done in CPU initialization
    // wrmsr(IA32_EFER, rdmsr(IA32_EFER) | BIT(0) /* SCE */);
    wrmsr(IA32_STAR, 0x0013000800000000); //  CS: 0x08, SS: 0x10, User CS: 0x1b, User SS: 0x23
    wrmsr(IA32_FSTAR, 0x43700); // Clear IF,TF,AC, and DF
    wrmsr(IA32_LSTAR, (uintptr_t)Arch_SyscallTrapHandler);
}
