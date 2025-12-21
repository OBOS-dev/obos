/*
 * oboskrnl/arch/x86_64/sse.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>

#include <arch/x86_64/asm_helpers.h>

#include <allocators/base.h>

static size_t xsave_size = 512;
bool Arch_HasXSAVE = false;

void* Arch_AllocateXSAVERegion()
{
#ifndef OBOS_DEBUG
    void* base = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, xsave_size, nullptr);
#else
    void* base = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, xsave_size+64, nullptr);
    base = (void*)(((uintptr_t)base + 63) & ~63);
#endif
    uint32_t* base32 = base;
    base32[0x18/4] = 0x1f80; // mxcsr
    return base;

}

void Arch_FreeXSAVERegion(void* buf)
{
#ifndef OBOS_DEBUG
    Free(OBOS_NonPagedPoolAllocator, buf, xsave_size);
#else
    OBOS_Debug("XSave Region 0x%p leaked!\n");
#endif
}

// Enables stuff such as XSAVE, SSE(2), AVX, AVX512, etc.
__attribute__((target("xsave"))) void Arch_EnableSIMDFeatures()
{
    // Enable SSE.

    // Clear CR0.EM, set CR0.MP
    asm ("mov %0, %%cr0" : :"r"(getCR0() & ~BIT(2)));
    asm ("mov %0, %%cr0" : :"r"(getCR0() | BIT(1)));
    // Set CR4.OSFXSR, CR4.OSXMMEXCPT
    asm ("mov %0, %%cr4" : :"r"(getCR4() | BIT(9) | BIT(10)));

    // Enable XSAVE, if supported
    uint32_t ecx = 0;
    __cpuid__(0x1, 0x0, nullptr, nullptr, &ecx, nullptr);
    if (ecx & BIT(26))
    {
        asm ("mov %0, %%cr4" : :"r"(getCR4() | BIT(18)));
        Arch_HasXSAVE = true;
        __cpuid__(0xd, 0, nullptr, nullptr, (uint32_t*)&xsave_size, nullptr);
    }

    // Enable AVX.
    if (ecx & BIT(28))
        __builtin_ia32_xsetbv(0, __builtin_ia32_xgetbv(0)|0b111);

    // Enable AVX512.
    uint32_t eax = 0;
    __cpuid__(0xd, 0, &eax, nullptr,nullptr,nullptr);
    if (eax & (0x7<<5))
        __builtin_ia32_xsetbv(0, __builtin_ia32_xgetbv(0) | (0x7<<5));
}

size_t Arch_GetXSaveRegionSize()
{return xsave_size;}
