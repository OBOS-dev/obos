/*
 * oboskrnl/arch/x86_64/syscall.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <syscall.h>

#include <scheduler/cpu_local.h>

#include <arch/x86_64/asm_helpers.h>

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083
#define IA32_FSTAR 0xC0000084

extern uint64_t Arch_cpu_local_currentKernelStack_offset;
extern void Arch_SyscallTrapHandler();
void OBOSS_InitializeSyscallInterface()
{
    // Enable IA32_EFER.SCE
    // This is done in CPU initialization
    // wrmsr(IA32_EFER, rdmsr(IA32_EFER) | BIT(0) /* SCE */);
    wrmsr(IA32_STAR, 0x0013000800000000); //  CS: 0x08, SS: 0x10, User CS: 0x1b, User SS: 0x23
    wrmsr(IA32_FSTAR, 0x43700); // Clear IF,TF,AC, and DF
    wrmsr(IA32_LSTAR, (uintptr_t)Arch_SyscallTrapHandler);
    Arch_cpu_local_currentKernelStack_offset = offsetof(cpu_local, currentKernelStack);
}