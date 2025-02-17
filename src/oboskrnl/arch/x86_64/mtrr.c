/*
 * oboskrnl/arch/x86_64/mtrr.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>

#include <allocators/base.h>

#include <arch/x86_64/mtrr.h>
#include <arch/x86_64/asm_helpers.h>

#include <scheduler/cpu_local.h>

#define IA32_MTRRCAP (0xFE)
#define IA32_MTRRdefType (0x2FF)
#define IA32_MTRRphysBase(n) (0x200 + (n) * 2)
#define IA32_MTRRphysMask(n) (0x201 + (n) * 2)
#define MTRRfix64K_00000 (0x250)
#define MTRRfix16K_80000 (0x258)
#define MTRRfix16K_A0000 (0x259)
#define MTRRfix4K_C0000 (0x268)
#define MTRRfix4K_C8000 (0x269)
#define MTRRfix4K_D0000 (0x26A)
#define MTRRfix4K_D8000 (0x26B)
#define MTRRfix4K_E0000 (0x26C)
#define MTRRfix4K_E8000 (0x26D)
#define MTRRfix4K_F0000 (0x26E)
#define MTRRfix4K_F8000 (0x26F)

static bool savedMtrrs = false;
static uint8_t nVariableMTRRs = 0;
static uint64_t *variableMTRRsBase;
static uint64_t *variableMTRRsMask;
static uint64_t deftype = 0;
static uint64_t fix64K = 0;
static uint64_t fix16K[2] = {};
static uint64_t fix4K[8] = {};
// Only to be called once on the BSP
void Arch_SaveMTRRs()
{
    OBOS_ASSERT(CoreS_GetCPULocalPtr()->isBSP);
    OBOS_ASSERT(!savedMtrrs);
    savedMtrrs = true;
    uint64_t cap = rdmsr(IA32_MTRRCAP);
    deftype = rdmsr(IA32_MTRRdefType);
    if (~deftype & BIT(11) /* enabled? */)
        return; // nope, abort.
    nVariableMTRRs = cap & 0xff;
    variableMTRRsBase = ZeroAllocate(OBOS_KernelAllocator, nVariableMTRRs, sizeof(uint64_t), nullptr);
    variableMTRRsMask = ZeroAllocate(OBOS_KernelAllocator, nVariableMTRRs, sizeof(uint64_t), nullptr);

    for (size_t i = 0; i < nVariableMTRRs; i++)
    {
        variableMTRRsBase[i] = rdmsr(IA32_MTRRphysBase(i));
        variableMTRRsMask[i] = rdmsr(IA32_MTRRphysMask(i));
    }

    if (deftype & BIT(8) /* fixed avaliable? */)
    {
        fix64K = rdmsr(MTRRfix64K_00000);
        fix16K[0] = rdmsr(MTRRfix16K_80000);
        fix16K[1] = rdmsr(MTRRfix16K_A0000);
        for (size_t i = 0; i < 8; i++)
            fix4K[i] = rdmsr(MTRRfix4K_C0000 + i);
    }
}
// Can be called on any CPU any amount of times.
void Arch_RestoreMTRRs()
{
    OBOS_ASSERT(savedMtrrs);
    if (~deftype & BIT(11) /* enabled? */)
        return; // nope, abort.

    for (size_t i = 0; i < nVariableMTRRs; i++)
    {
        wrmsr(IA32_MTRRphysBase(i), variableMTRRsBase[i]);
        wrmsr(IA32_MTRRphysMask(i), variableMTRRsMask[i] );
    }

    if (deftype & BIT(8) /* fixed avaliable? */)
    {
        wrmsr(MTRRfix64K_00000, fix64K );
        wrmsr(MTRRfix16K_80000, fix16K[0]);
        wrmsr(MTRRfix16K_A0000, fix16K[1]);
        for (size_t i = 0; i < 8; i++)
            wrmsr(MTRRfix4K_C0000 + i, fix4K[i]);
    }
}
