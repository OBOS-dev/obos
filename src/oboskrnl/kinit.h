/*
 * oboskrnl/kinit.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>

struct boot_module
{
    const char* name;
    void* address;
    size_t size;
    bool is_memory;
    bool is_kernel;
};

// Called after CPU-local data is initialized
void OBOS_KernelInit();
void OBOS_LoadSymbolTable();
OBOS_WEAK void OBOSS_KernelPostPMMInit();
OBOS_WEAK void OBOSS_KernelPostVMMInit();
OBOS_WEAK void OBOSS_KernelPostTmInit();
OBOS_WEAK void OBOSS_KernelPostKProcInit();
OBOS_WEAK void OBOSS_KernelPostIRQInit();
OBOS_WEAK void OBOSS_InitializeSMP();
OBOS_WEAK void OBOSS_MakeTTY();
void OBOSS_GetModule(struct boot_module *module, const char* name);
void OBOSS_GetModuleLen(struct boot_module *module, const char* name, size_t name_len);
void OBOSS_GetKernelModule(struct boot_module *module);

#if defined (__x86_64__)
#   define OBOS_DEFAULT_UACPI_INTERRUPT_MODEL UACPI_INTERRUPT_MODEL_IOAPIC
#endif