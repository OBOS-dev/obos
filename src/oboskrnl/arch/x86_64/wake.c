/*
 * oboskrnl/arch/x86_64/wake.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <syscall.h>
#include <cmdline.h>

#include <stdatomic.h>

#include <mm/pmm.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/pmm.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/hpet_table.h>
#include <arch/x86_64/mtrr.h>
#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/sse.h>
#include <arch/x86_64/boot_info.h>

#include <UltraProtocol/ultra_protocol.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <allocators/base.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <irq/irql.h>

#include <power/suspend.h>

struct {
    HPET_Timer timer0;
    uint64_t mainCounterValue;
    uint64_t generalConfig;
} hpet_state;
struct ioapic_state
{
    uint64_t* redirection_entries;
    uint8_t arbitrationId;
    uint8_t id;
};
struct ioapic_state* saved_ioapics; // one to one indexing with Arch_IOAPICs
#if OBOS_NVS_SAVE_S3
struct acpi_nvs {
    void* saved_region;
    void* region_address;
    size_t size;
};
struct acpi_nvs* saved_nvs;
size_t saved_nvs_count;
#endif
void OBOSS_SuspendSavePlatformState()
{
    Arch_HPETAddress->generalConfig &= ~BIT(0);
    hpet_state.mainCounterValue = Arch_HPETAddress->mainCounterValue;
    hpet_state.generalConfig = Arch_HPETAddress->generalConfig | 1;
    hpet_state.timer0.timerComparatorValue = Arch_HPETAddress->timer0.timerComparatorValue;
    hpet_state.timer0.timerFSBInterruptRoute = Arch_HPETAddress->timer0.timerFSBInterruptRoute;
    hpet_state.timer0.timerConfigAndCapabilities = Arch_HPETAddress->timer0.timerConfigAndCapabilities;
    Arch_HPETAddress->generalConfig |= BIT(0);

    saved_ioapics = ZeroAllocate(OBOS_NonPagedPoolAllocator, Arch_IOAPICCount, sizeof(struct ioapic_state), nullptr);
    for (size_t i = 0; i < Arch_IOAPICCount; i++)
    {
        saved_ioapics[i].arbitrationId = (ArchH_IOAPICReadRegister(Arch_IOAPICs[i].address, 8) >> 24) & 0xf;
        saved_ioapics[i].id = (ArchH_IOAPICReadRegister(Arch_IOAPICs[i].address, 0) >> 24) & 0xf;
        saved_ioapics[i].redirection_entries = ZeroAllocate(OBOS_NonPagedPoolAllocator,
                                                                                        Arch_IOAPICs[i].maxRedirectionEntries,
                                                                                        sizeof(uint64_t),
                                                                                        nullptr);
        for (uint8_t entry = 0; entry < Arch_IOAPICs[i].maxRedirectionEntries; entry++)
        {
            uint64_t val = 0;
            val = ArchH_IOAPICReadRegister(Arch_IOAPICs[i].address, 0x40+entry*8);
            val |= ((uint64_t)ArchH_IOAPICReadRegister(Arch_IOAPICs[i].address, 0x44+entry*8)) << 32;
            saved_ioapics[i].redirection_entries[entry] = val;
        }
    }

    // Save NVS, if requested.
#if OBOS_NVS_SAVE_S3
    if (!OBOS_GetOPTF("nvs-nosave-s3"))
    {
        // Save ACPI NVS regions.
        // Why? Some versions of windows do, so I guess we have to :)

        // Read the memory map.
        uintptr_t index = 0;
        for (obos_pmem_map_entry* entry = MmS_GetFirstPMemMapEntry(&index); entry; )
        {
            if (entry->type == ULTRA_MEMORY_TYPE_NVS)
                saved_nvs_count++;

            entry = MmS_GetNextPMemMapEntry(entry, &index);
        }

        saved_nvs = ZeroAllocate(OBOS_NonPagedPoolAllocator, saved_nvs_count, sizeof(struct acpi_nvs), nullptr);

        index = 0;
        size_t nvs_index = 0;
        for (obos_pmem_map_entry* entry = MmS_GetFirstPMemMapEntry(&index); entry; )
        {
            if (entry->type == ULTRA_MEMORY_TYPE_NVS)
            {
                saved_nvs[nvs_index].region_address = Arch_MapToHHDM(entry->physical_address);
                saved_nvs[nvs_index].size = entry->size;
                saved_nvs[nvs_index].saved_region = Allocate(OBOS_NonPagedPoolAllocator,
                                                                                         saved_nvs[nvs_index].size,
                                                                                         nullptr);
                memcpy(saved_nvs[nvs_index].saved_region, saved_nvs[nvs_index].region_address, saved_nvs[nvs_index].size);
                nvs_index++;
            }

            entry = MmS_GetNextPMemMapEntry(entry, &index);
        }
    }
#endif
}

static void restore_nvs()
{
#if OBOS_NVS_SAVE_S3
    for (size_t i = 0; i < saved_nvs_count; i++)
    {
        memcpy(saved_nvs[i].region_address, saved_nvs[i].saved_region, saved_nvs[i].size);
        Free(OBOS_NonPagedPoolAllocator, saved_nvs[i].saved_region, saved_nvs[i].size);
    }
    Free(OBOS_NonPagedPoolAllocator, saved_nvs, saved_nvs_count*sizeof(struct acpi_nvs));
    saved_nvs_count = 0;
#endif
}

static void restore_hpet()
{
    Arch_HPETAddress->mainCounterValue = hpet_state.mainCounterValue;
    Arch_HPETAddress->timer0.timerComparatorValue = hpet_state.timer0.timerComparatorValue;
    Arch_HPETAddress->timer0.timerFSBInterruptRoute = hpet_state.timer0.timerFSBInterruptRoute;
    Arch_HPETAddress->timer0.timerConfigAndCapabilities = hpet_state.timer0.timerConfigAndCapabilities;
    Arch_HPETAddress->generalConfig = hpet_state.generalConfig;
}

static void restore_ioapics()
{
    for (size_t i = 0; i < Arch_IOAPICCount; i++)
    {
        uint32_t tmp = 0;

        tmp = ((uint32_t)saved_ioapics[i].id) << 24;
        ArchH_IOAPICWriteRegister(Arch_IOAPICs[i].address, 0x0, tmp);

        tmp = ((uint32_t)saved_ioapics[i].arbitrationId) << 24;
        ArchH_IOAPICWriteRegister(Arch_IOAPICs[i].address, 0x8, tmp);

        for (uint8_t entry = 0; entry < Arch_IOAPICs[i].maxRedirectionEntries; entry++)
        {
            tmp = saved_ioapics[i].redirection_entries[entry] >> 32;
            ArchH_IOAPICWriteRegister(Arch_IOAPICs[i].address, 0x44+entry*8, tmp);
            tmp = saved_ioapics[i].redirection_entries[entry] & 0xffffffff;
            ArchH_IOAPICWriteRegister(Arch_IOAPICs[i].address, 0x40+entry*8, tmp);
        }
        Free(OBOS_NonPagedPoolAllocator, saved_ioapics[i].redirection_entries, Arch_IOAPICs[i].maxRedirectionEntries*sizeof(uint64_t));
    }
    Free(OBOS_NonPagedPoolAllocator, saved_ioapics, Arch_IOAPICCount*sizeof(struct ioapic_state));
}

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
#ifdef OBOS_UP
static void restart_cpus()
{}
#else
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
#endif

extern void Arch_disablePIC();

extern uint64_t Arch_FindCounter(uint64_t hz);
static void __attribute__((no_stack_protector)) on_wake(cpu_local* info)
{
    static uint64_t cached_counter;
    struct
    {
        uint16_t limit;
        uintptr_t base;
    } OBOS_PACK gdtr;
    gdtr.limit = sizeof(info->arch_specific.gdtEntries) - 1;
    gdtr.base = (uintptr_t)&info->arch_specific.gdtEntries;
    wrmsr(0xC0000101 /* GS_BASE */, (uint64_t)info);
    reinit_tss_segment(info);
    Arch_FlushGDT((uintptr_t)&gdtr);
    wrmsr(0xC0000101 /* GS_BASE */, (uint64_t)info);
    irql oldIrql = Core_RaiseIrqlNoThread(0xf);
    OBOS_UNUSED(oldIrql);
    Arch_InitializeMiscFeatures();
    Arch_EnableSIMDFeatures();
    Arch_RestoreMTRRs();
    // UC UC- WT WB UC WC WT WB
    wrmsr(0x277, 0x0001040600070406);
    asm volatile("mov %0, %%cr3" : :"r"(getCR3()));
    wbinvd();
    Arch_LAPICInitialize(info->isBSP);
    wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080)|BIT(0));
    Arch_InitializeIDT(false);
    OBOSS_InitializeSyscallInterface();
    if (info->isBSP)
    {
        Arch_disablePIC();
        // Restore NVS
        restore_nvs();
        // Restore IOAPICs.
        restore_ioapics();
        // Restore the HPET.
        restore_hpet();
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
    if (info->isBSP)
    {
        OBOS_WokeFromSuspend = true;
        Core_SuspendScheduler(false);
    }
    if (CoreS_GetCPULocalPtr()->currentThread == OBOS_SuspendWorkerThread)
        OBOS_SuspendWorkerThread->context.frame.rflags &= 0x200; // enter with interrupts disabled
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
