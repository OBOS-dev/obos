/*
 * oboskrnl/arch/x86_64/smp.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <stdatomic.h>

#include <arch/x86_64/lapic.h>
#include <arch/x86_64/interrupt_frame.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/sdt.h>
#include <arch/x86_64/madt.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <mm/bare_map.h>
#include <mm/context.h>

#include <arch/x86_64/idt.h>

#include <arch/x86_64/boot_info.h>

#include <arch/x86_64/pmm.h>

#include <allocators/base.h>

#include <scheduler/process.h>
#include <stdint.h>

static uint8_t s_lapicIDs[256];
static uint8_t s_nLAPICIDs = 0;
#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
#define NextMADTEntry(cur) OffsetPtr(cur, cur->length, MADT_EntryHeader)
obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
static OBOS_NO_UBSAN void ParseMADT()
{
	// Find the MADT in the ACPI tables.
	ACPIRSDPHeader* rsdp = (ACPIRSDPHeader*)Arch_MapToHHDM(Arch_LdrPlatformInfo->acpi_rsdp_address);
	bool tables32 = rsdp->Revision == 0;
	ACPISDTHeader* xsdt = tables32 ? (ACPISDTHeader*)(uintptr_t)rsdp->RsdtAddress : (ACPISDTHeader*)rsdp->XsdtAddress;
	xsdt = (ACPISDTHeader*)Arch_MapToHHDM((uintptr_t)xsdt);
	size_t nEntries = (xsdt->Length - sizeof(*xsdt)) / (tables32 ? 4 : 8);
	MADTTable* madt = nullptr;
	for (size_t i = 0; i < nEntries; i++)
	{
		uintptr_t phys = tables32 ? OffsetPtr(xsdt, sizeof(*xsdt), uint32_t)[i] : OffsetPtr(xsdt, sizeof(*xsdt), uint64_t)[i];
		ACPISDTHeader* header = (ACPISDTHeader*)Arch_MapToHHDM(phys);
		if (memcmp(header->Signature, "APIC", 4))
		{
			madt = (MADTTable*)header;
			break;
		}
	}
	void* end = OffsetPtr(madt, madt->sdtHeader.Length, void);
	for (MADT_EntryHeader* cur = OffsetPtr(madt, sizeof(*madt), MADT_EntryHeader); (uintptr_t)cur < (uintptr_t)end; cur = NextMADTEntry(cur))
	{
		if (cur->type == 0)
		{
			MADT_EntryType0* mLapicId = (MADT_EntryType0*)cur;
			if (s_nLAPICIDs == 255)
				break; // make continue if more types are parsed
			s_lapicIDs[s_nLAPICIDs++] = mLapicId->apicID;
		}
	}
}
extern uint8_t Arch_SMPTrampolineStart[];
extern uint8_t Arch_SMPTrampolineEnd[];
extern uint64_t Arch_SMPTrampolineCR3;
extern uint64_t Arch_SMPTrampolineRSP;
extern uint64_t Arch_SMPTrampolineCPULocalPtr;
static _Atomic(bool) ap_initialized;
static void nmiHandler(interrupt_frame* frame);
extern void Arch_FlushGDT(uintptr_t gdtr);
void Arch_CPUInitializeGDT(cpu_local *info, uintptr_t istStack, size_t istStackSize)
{
	memzero(info->arch_specific.gdtEntries, sizeof(info->arch_specific.gdtEntries));
	memzero(&info->arch_specific.tss, sizeof(info->arch_specific.tss));
	info->arch_specific.gdtEntries[0] = 0;
	info->arch_specific.gdtEntries[1] = 0x00af9b000000ffff; // 64-bit code
	info->arch_specific.gdtEntries[2] = 0x00cf93000000ffff; // 64-bit data
	info->arch_specific.gdtEntries[3] = 0x00aff3000000ffff; // 64-bit user-mode data
	info->arch_specific.gdtEntries[4] = 0x00cff3000000ffff; // 64-bit user-mode code
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

	info->arch_specific.tss.ist0 = istStack + istStackSize;
	info->arch_specific.tss.rsp0 = istStack + istStackSize;
	info->arch_specific.tss.iopb = sizeof(info->arch_specific.tss) - 1;
	struct
	{
		uint16_t limit;
		uintptr_t base;
	} OBOS_PACK gdtr;
	gdtr.limit = sizeof(info->arch_specific.gdtEntries) - 1;
	gdtr.base = info->arch_specific.gdtEntries;
	Arch_FlushGDT(&gdtr);
}
extern void Arch_IdleTask();
OBOS_NORETURN extern void Arch_APYield(void* startup_stack, void* temp_stack);
static void idleTaskBootstrap()
{
	ap_initialized = true;
	CoreS_GetCPULocalPtr()->initialized = true;
	Arch_IdleTask();
}
void Arch_APEntry(cpu_local* info)
{
	Arch_CPUInitializeGDT(info, (uintptr_t)info->arch_specific.ist_stack, 0x10000);
	wrmsr(0xC0000101 /* GS_BASE */, (uint64_t)info);
	Arch_InitializeIDT(false);
	(void)Core_RaiseIrql(0xf);
	// Setup the idle thread.
	thread_ctx ctx;
	memzero(&ctx, sizeof(ctx));
	void* thr_stack = OBOS_BasicMMAllocatePages(0x4000, nullptr);
	CoreS_SetupThreadContext(&ctx, idleTaskBootstrap, 0, false, thr_stack, 0x4000);
	thread* idleThread = CoreH_ThreadAllocate(nullptr);
	CoreH_ThreadInitialize(idleThread, THREAD_PRIORITY_IDLE, ((thread_affinity)1<<info->id), &ctx);
	CoreH_ThreadReady(idleThread);
	Core_ProcessAppendThread(OBOS_KernelProcess, idleThread);
	info->idleThread = idleThread;
	Arch_LAPICInitialize(false);
	Arch_APYield(info->arch_specific.startup_stack, info->arch_specific.ist_stack);
}
static OBOS_NO_UBSAN void SetMemberInSMPTrampoline(uint8_t off, uint64_t val)
{
	*OffsetPtr(nullptr, off, uint64_t) = val;
}
static OBOS_NO_UBSAN uint64_t GetMemberInSMPTrampoline(uint8_t off)
{
	return *OffsetPtr(nullptr, off, uint64_t);
}
OBOS_EXCLUDE_VAR_FROM_MM bool Arch_SMPInitialized = false;
void Arch_SMPStartup()
{
	Arch_RawRegisterInterrupt(0x2, (uintptr_t)nmiHandler);
	ParseMADT();
#ifdef OBOS_UP
	OBOS_Log("Uniprocessor-build of OBOS. No other cores will be initialized.\n");
	s_nLAPICIDs = 1;
#endif
	// if (s_nLAPICIDs == 1)
	// 	return; // No work to do.
	// ^ is not true
	cpu_local* cpu_info = (cpu_local*)Mm_VMMAllocator->Allocate(Mm_VMMAllocator, s_nLAPICIDs*sizeof(cpu_local), nullptr);
	memzero(cpu_info, s_nLAPICIDs * sizeof(cpu_local));
	OBOS_STATIC_ASSERT(sizeof(*cpu_info) == sizeof(*Core_CpuInfo), "Size mismatch for Core_CpuInfo and cpu_info.");
	cpu_info[0] = Core_CpuInfo[0];
	Arch_MapPage(getCR3(), nullptr, 0, 0x3);
	Arch_SMPTrampolineCR3 = getCR3();
	Core_CpuInfo = cpu_info;
	Core_CpuCount = s_nLAPICIDs;
	irql oldIrql = Core_RaiseIrql(0xf);
	Core_CpuTempStackSize = 0x10000;
	void* temp_stacks = Core_CpuTempStackBase =  OBOS_BasicMMAllocatePages(Core_CpuTempStackSize * Core_CpuCount, nullptr);
	for (size_t i = 0; i < s_nLAPICIDs; i++)
	{
		if (s_lapicIDs[i] == Arch_LAPICAddress->lapicID)
		{
			Arch_CPUInitializeGDT(&cpu_info[i], ((uintptr_t)temp_stacks + i*Core_CpuTempStackSize), 0x10000);
			wrmsr(0xC0000101 /* GS_BASE */, (uintptr_t)&cpu_info[0]);
			continue;
		}
		memcpy(nullptr, Arch_SMPTrampolineStart, Arch_SMPTrampolineEnd - Arch_SMPTrampolineStart);
		for (thread_priority j = 0; j <= THREAD_PRIORITY_MAX_VALUE; j++)
			cpu_info[i].priorityLists[j].priority = j;
		cpu_info[i].id = s_lapicIDs[i];
		cpu_info[i].currentIrql = 0;
		cpu_info[i].isBSP = false;
		cpu_info[i].schedulerTicks = 0;
		cpu_info[i].arch_specific.ist_stack = (void*)((uintptr_t)temp_stacks + i*Core_CpuTempStackSize);
		cpu_info[i].arch_specific.startup_stack = OBOS_BasicMMAllocatePages(0x4000, nullptr);
		Core_DefaultThreadAffinity |= CoreH_CPUIdToAffinity(cpu_info[i].id);
		SetMemberInSMPTrampoline((uintptr_t)&Arch_SMPTrampolineRSP - (uintptr_t)Arch_SMPTrampolineStart, (uint64_t)cpu_info[i].arch_specific.startup_stack + 0x4000);
		SetMemberInSMPTrampoline((uintptr_t)&Arch_SMPTrampolineCPULocalPtr - (uintptr_t)Arch_SMPTrampolineStart, (uint64_t)&cpu_info[i]);
		ipi_lapic_info lapic = {
			.isShorthand = false,
			.info = {
				.lapicId = s_lapicIDs[i]
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
		vector.info.address = 0x0000;
		if ((status = Arch_LAPICSendIPI(lapic, vector)) != OBOS_STATUS_SUCCESS)
		{
			OBOS_Error("%s: Could not send IPI. Status: %d.\n", status);
			continue;
		}
		while (!atomic_load(&ap_initialized))
			pause();
		atomic_store(&ap_initialized, false);
	}
	Core_LowerIrql(oldIrql);
	Arch_SMPInitialized = true;
	OBOSS_UnmapPage(nullptr);
}
OBOS_EXCLUDE_VAR_FROM_MM bool Arch_HaltCPUs = false;
OBOS_EXCLUDE_VAR_FROM_MM uint8_t Arch_CPUsHalted = 0;
bool Arch_InvlpgIPI(interrupt_frame* frame);
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM static void nmiHandler(interrupt_frame* frame)
{
	if (Arch_InvlpgIPI(frame))
		return;
	if (Arch_HaltCPUs)
	{
		atomic_fetch_add(&Arch_CPUsHalted, 1);
		cli();
		while (1) 
			hlt();
	}
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Unhandled NMI!\n");
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM static void HaltInitializedCPUs()
{
	Arch_HaltCPUs = true;
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		if (!Core_CpuInfo[i].initialized)
			continue;
		ipi_lapic_info lapic = {
			.isShorthand = false,
			.info = {
				.lapicId = Core_CpuInfo[i].id,
			},
		};
		ipi_vector_info vector = {
			.deliveryMode = LAPIC_DELIVERY_MODE_NMI,
		};
		Arch_LAPICSendIPI(lapic, vector);
	}
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM void OBOSS_HaltCPUs()
{
	if (Core_CpuCount == 1)
		return;
	if (!Arch_SMPInitialized)
	{
		HaltInitializedCPUs();
		return;
	}
	ipi_lapic_info lapic = {
		.isShorthand = true,
		.info = {
			.shorthand = LAPIC_DESTINATION_SHORTHAND_ALL_BUT_SELF,
		},
	};
	ipi_vector_info vector = {
		.deliveryMode = LAPIC_DELIVERY_MODE_NMI,
	};
	Arch_HaltCPUs = true;
	Arch_LAPICSendIPI(lapic, vector);
	// Wait for all CPUs to halt.
	while (atomic_load(&Arch_CPUsHalted) != (Core_CpuCount - 1))
		pause();
}
uintptr_t Arch_GetCPUTempStack()
{
	return CoreS_GetCPULocalPtr()->arch_specific.ist_stack;
}