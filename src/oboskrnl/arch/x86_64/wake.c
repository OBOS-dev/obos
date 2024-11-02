/*
 * oboskrnl/arch/x86_64/wake.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <syscall.h>
#include <stdatomic.h>

#include <mm/pmm.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/pmm.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/idt.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <irq/irql.h>

#include <power/suspend.h>

extern uint8_t Arch_SMPTrampolineStart[];
extern uint8_t Arch_SMPTrampolineEnd[];
extern uint64_t Arch_SMPTrampolineCR3;
extern uint64_t Arch_SMPTrampolineRSP;
extern uint64_t Arch_SMPTrampolineCPULocalPtr;

#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
static OBOS_NO_UBSAN void SetMemberInSMPTrampoline(uint8_t off, uint64_t val)
{
    *OffsetPtr(0x1000, off, uint64_t) = val;
}

obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool free_pte);
uint32_t OBOSS_WakeVector = 0;
extern char Arch_ACPIWakeTrampoline;
extern char Arch_ACPIWakeTrampoline_data;
extern char Arch_ACPIWakeTrampoline_end;
extern uintptr_t Arch_SMPTrampolineWakeLocation;
extern void Arch_FlushGDT(uintptr_t gdtr);
static void reinit_tss_segment(cpu_local* info)
{
    struct
    {
        uint16_t limitLow;
        uint16_t baseLow;
        uint8_t baseMiddle1;
        uint8_t access;
        uint8_t gran;
        uint8_t baseMiddle2;
        uint32_t baseHigh;
        uint32_t resv1;
    } tss_entry;
    memzero(&tss_entry, sizeof(tss_entry));
    uintptr_t tss = (uintptr_t)&info->arch_specific.tss;
    tss_entry.limitLow = sizeof(info->arch_specific.tss);
    tss_entry.baseLow = tss & 0xffff;
    tss_entry.baseMiddle1 = (tss >> 16) & 0xff;
    tss_entry.baseMiddle2 = (tss >> 24) & 0xff;
    tss_entry.baseHigh = (tss >> 32) & 0xffffffff;
    tss_entry.access = 0x89;
    tss_entry.gran = 0x40;
    info->arch_specific.gdtEntries[5] = *((uint64_t*)&tss_entry + 0);
    info->arch_specific.gdtEntries[6] = *((uint64_t*)&tss_entry + 1);
}
extern void Arch_InitializeMiscFeatures();
atomic_bool ap_initialized;
static void restart_cpus()
{
    for (size_t i = 0; i < Core_CpuCount; i++)
    {
        if (Core_CpuInfo[i].isBSP)
            continue;
        memcpy((void*)0x1000, Arch_SMPTrampolineStart, Arch_SMPTrampolineEnd - Arch_SMPTrampolineStart);
        Core_CpuInfo[i].arch_specific.startup_stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
        SetMemberInSMPTrampoline((uintptr_t)&Arch_SMPTrampolineRSP - (uintptr_t)Arch_SMPTrampolineStart, (uint64_t)Core_CpuInfo[i].arch_specific.startup_stack + 0x4000);
        SetMemberInSMPTrampoline((uintptr_t)&Arch_SMPTrampolineCPULocalPtr - (uintptr_t)Arch_SMPTrampolineStart, (uint64_t)&Core_CpuInfo[i]);
        ipi_lapic_info lapic = {
            .isShorthand = false,
            .info = {
                .lapicId = Core_CpuInfo[i].id
            },
        };
        ipi_vector_info vector = {
            .deliveryMode = LAPIC_DELIVERY_MODE_INIT,
            .info = {
                .vector = 0,
            },
        };
        obos_status status = 0;
        if ((status = Arch_LAPICSendIPI(lapic, vector)) != OBOS_STATUS_SUCCESS)
        {
            OBOS_Error("%s: Could not send IPI. Status: %d.\n", status);
            continue;
        }
        vector.deliveryMode = LAPIC_DELIVERY_MODE_SIPI;
        vector.info.address = 0x1000;
        if ((status = Arch_LAPICSendIPI(lapic, vector)) != OBOS_STATUS_SUCCESS)
        {
            OBOS_Error("%s: Could not send IPI. Status: %d.\n", status);
            continue;
        }
        while (!atomic_load(&ap_initialized))
            pause();
        atomic_store(&ap_initialized, false);
    }
}
extern uint64_t Arch_FindCounter(uint64_t hz);
static void on_wake(cpu_local* info)
{
    static uint64_t cached_counter;
    struct
    {
        uint16_t limit;
        uintptr_t base;
    } OBOS_PACK gdtr;
    gdtr.limit = sizeof(info->arch_specific.gdtEntries) - 1;
    gdtr.base = (uintptr_t)&info->arch_specific.gdtEntries;
    reinit_tss_segment(info);
    Arch_InitializeIDT(false);
    Arch_FlushGDT((uintptr_t)&gdtr);
    wrmsr(0xC0000101 /* GS_BASE */, (uint64_t)info);
    irql oldIrql = Core_RaiseIrqlNoThread(0xf);
    OBOS_UNUSED(oldIrql);
    Arch_LAPICInitialize(info->isBSP);
    Arch_InitializeMiscFeatures();
    // UC UC- WT WB UC WC WT WB
    wrmsr(0x277, 0x0001040600070406);
    asm volatile("mov %0, %%cr3" : :"r"(getCR3()));
    wbinvd();
    wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080)|BIT(0));
    OBOSS_InitializeSyscallInterface();
    if (info->isBSP)
    {
        // Restart CPUs.
        restart_cpus();
        cached_counter = Arch_FindCounter(Core_SchedulerTimerFrequency);
    }
    else
        atomic_store(&ap_initialized, true);
    while (!cached_counter)
        OBOSS_SpinlockHint();
    Arch_LAPICAddress->lvtTimer = 0x20000 | (Core_SchedulerIRQ->vector->id + 0x20);
    Arch_LAPICAddress->initialCount = cached_counter;
    Arch_LAPICAddress->divideConfig = 0xB;
    OBOS_Debug("Reinitialized timer for CPU %d.\n", CoreS_GetCPULocalPtr()->id);
    if (info->isBSP)
    {
        OBOS_WokeFromSuspend = true;
        Core_SuspendScheduler(false);
    }
    CoreS_SwitchToThreadContext(&CoreS_GetCPULocalPtr()->currentThread->context);
}
obos_status OBOSS_PrepareWakeVector()
{
    OBOSS_WakeVector = 0x1000;
    Arch_MapPage(getCR3(), (void*)(uintptr_t)OBOSS_WakeVector, OBOSS_WakeVector, 0x3, false);
    Arch_SMPTrampolineCR3 = getCR3();
    Arch_SMPTrampolineCPULocalPtr = (uintptr_t)&Core_CpuInfo[0];
    Arch_SMPTrampolineRSP = (uintptr_t)Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
    Arch_SMPTrampolineRSP += 0x4000;
    memcpy((void*)(uintptr_t)OBOSS_WakeVector, &Arch_SMPTrampolineStart, Arch_SMPTrampolineEnd - Arch_SMPTrampolineStart);
    Arch_SMPTrampolineWakeLocation = (uintptr_t)on_wake;
    return OBOS_STATUS_SUCCESS;
}
