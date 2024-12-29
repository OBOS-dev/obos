/*
 * oboskrnl/arch/x86_64/smp.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <syscall.h>
#include <cmdline.h>

#include <stdatomic.h>

#include <arch/x86_64/lapic.h>
#include <arch/x86_64/interrupt_frame.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/sdt.h>
#include <arch/x86_64/madt.h>
#include <arch/x86_64/mtrr.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>

#include <mm/bare_map.h>

#include <arch/x86_64/idt.h>

#include <arch/x86_64/boot_info.h>

#include <arch/x86_64/pmm.h>

#include <allocators/base.h>

#include <scheduler/process.h>

#include <arch/x86_64/gdbstub/debug.h>

#include <irq/irq.h>
#include <irq/irql.h>

static uint8_t s_lapicIDs[256];
static uint8_t s_nLAPICIDs = 0;
#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
#define NextMADTEntry(cur) OffsetPtr(cur, cur->length, MADT_EntryHeader)
obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool);
static OBOS_NO_UBSAN void ParseMADT()
{
	// Find the MADT in the ACPI tables.
	ACPIRSDPHeader* rsdp = (ACPIRSDPHeader*)Arch_MapToHHDM(Arch_LdrPlatformInfo->acpi_rsdp_address);
	bool tables32 = rsdp->Revision < 2;
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
			if (~mLapicId->flags & BIT(0))
				continue;
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
	info->arch_specific.gdtEntries[3] = 0x00cff3000000ffff; // 64-bit user-mode data
	info->arch_specific.gdtEntries[4] = 0x00affb000000ffff; // 64-bit user-mode code
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
	tss_entry.limitLow = sizeof(info->arch_specific.tss)-1;
	tss_entry.baseLow = tss & 0xffff;
	tss_entry.baseMiddle1 = (tss >> 16) & 0xff;
	tss_entry.baseMiddle2 = (tss >> 24) & 0xff;
	tss_entry.baseHigh = (tss >> 32) & 0xffffffff;
	tss_entry.access = 0x89;
	tss_entry.gran = 0x40;
	info->arch_specific.gdtEntries[5] = *((uint64_t*)&tss_entry + 0);
	info->arch_specific.gdtEntries[6] = *((uint64_t*)&tss_entry + 1);

	info->arch_specific.tss.ist0 = istStack + 0x10000 /* see comment as to why we do this */;
	info->arch_specific.tss.rsp0 = istStack + 0x20000 /* see comment as to why we do this */;
	info->arch_specific.tss.iopb = sizeof(info->arch_specific.tss) - 1;
	struct
	{
		uint16_t limit;
		uintptr_t base;
	} OBOS_PACK gdtr;
	gdtr.limit = sizeof(info->arch_specific.gdtEntries) - 1;
	gdtr.base = (uintptr_t)&info->arch_specific.gdtEntries;
	Arch_FlushGDT((uintptr_t)&gdtr);
	wrmsr(0xC0000101, (uintptr_t)info);
}
extern void Arch_IdleTask();
OBOS_NORETURN extern void Arch_APYield(void* startup_stack, void* temp_stack);
static void idleTaskBootstrap()
{
	ap_initialized = true;
	CoreS_GetCPULocalPtr()->initialized = true;
	Arch_IdleTask();
}
extern void Arch_InitializeMiscFeatures();
void __attribute__((no_stack_protector)) Arch_APEntry(cpu_local* info)
{
	wrmsr(0xC0000101 /* GS_BASE */, (uint64_t)info);
	Arch_CPUInitializeGDT(info, (uintptr_t)info->arch_specific.ist_stack, 0x20000);
	Arch_InitializeIDT(false);
	irql oldIrql = Core_RaiseIrql(0xf);
	OBOS_UNUSED(oldIrql);
	// Setup the idle thread.
	thread_ctx ctx;
	memzero(&ctx, sizeof(ctx));
	void* thr_stack = OBOS_BasicMMAllocatePages(0x10000, nullptr);
	CoreS_SetupThreadContext(&ctx, (uintptr_t)idleTaskBootstrap, 0, false, thr_stack, 0x10000);
	thread* idleThread = CoreH_ThreadAllocate(nullptr);
	CoreH_ThreadInitialize(idleThread, THREAD_PRIORITY_IDLE, ((thread_affinity)1<<info->id), &ctx);
	CoreH_ThreadReady(idleThread);
	Core_ProcessAppendThread(OBOS_KernelProcess, idleThread);
	info->idleThread = idleThread;
	Arch_LAPICInitialize(false);
	Arch_InitializeMiscFeatures();
	// UC UC- WT WB UC WC WT WB
	wrmsr(0x277, 0x0001040600070406);
	Arch_RestoreMTRRs();
	asm volatile("mov %0, %%cr3" : :"r"(getCR3()));
	wbinvd();
	wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080)|BIT(0));
	OBOSS_InitializeSyscallInterface();
	Arch_APYield(info->arch_specific.startup_stack, info->arch_specific.ist_stack);
}
static OBOS_NO_UBSAN void SetMemberInSMPTrampoline(uint8_t off, uint64_t val)
{
	*OffsetPtr(0x1000, off, uint64_t) = val;
}
// static OBOS_NO_UBSAN uint64_t GetMemberInSMPTrampoline(uint8_t off)
// {
// 	return *OffsetPtr(nullptr, off, uint64_t);
// }
bool Arch_SMPInitialized = false;
void Arch_SMPStartup()
{
	Arch_SaveMTRRs();
	Arch_RawRegisterInterrupt(0x2, (uintptr_t)nmiHandler);
	ParseMADT();
#ifdef OBOS_UP
	OBOS_Log("Uniprocessor-build of OBOS. No other cores will be initialized.\n");
	s_nLAPICIDs = 1;
#else
	if (OBOS_GetOPTF("no-smp"))
	{
		OBOS_Log("Running OBOS as Uniprocessor. No other cores will be initialized.\n");
		s_nLAPICIDs = 1;
	}
#endif
	// if (s_nLAPICIDs == 1)
	// 	return; // No work to do.
	//             ^ is not true
	cpu_local* cpu_info = (cpu_local*)OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, s_nLAPICIDs, sizeof(cpu_local), nullptr);
	memzero(cpu_info, s_nLAPICIDs * sizeof(cpu_local));
	OBOS_STATIC_ASSERT(sizeof(*cpu_info) == sizeof(*Core_CpuInfo), "Size mismatch for Core_CpuInfo and cpu_info.");
	cpu_info[0] = Core_CpuInfo[0];
	cpu_info[0].currentPriorityList = cpu_info[0].priorityLists + (Core_CpuInfo[0].currentPriorityList - Core_CpuInfo[0].priorityLists);
	Arch_MapPage(getCR3(), (void*)0x1000, 0x1000, 0x3, false);
	Arch_SMPTrampolineCR3 = getCR3();
	Core_CpuInfo = cpu_info;
	Core_CpuCount = s_nLAPICIDs;
	irql oldIrql = Core_RaiseIrql(0xf);
	for (size_t i = 0; i < s_nLAPICIDs; i++)
	{
		if (s_lapicIDs[i] == Arch_LAPICAddress->lapicID)
		{
			Arch_CPUInitializeGDT(&cpu_info[i], (uintptr_t)(cpu_info[i].arch_specific.ist_stack = OBOS_BasicMMAllocatePages(0x20000, nullptr)), 0x20000);
			wrmsr(0xC0000101 /* GS_BASE */, (uintptr_t)&cpu_info[0]);
			// UC UC- WT WB UC WC WT WB
			wrmsr(0x277, 0x0001040600070406);
			asm volatile("mov %0, %%cr3" : :"r"(getCR3()));
			wbinvd();
			wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080)|BIT(0));
			OBOSS_InitializeSyscallInterface();
			continue;
		}
		memcpy((void*)0x1000, Arch_SMPTrampolineStart, Arch_SMPTrampolineEnd - Arch_SMPTrampolineStart);
		for (thread_priority j = 0; j <= THREAD_PRIORITY_MAX_VALUE; j++)
			cpu_info[i].priorityLists[j].priority = j;
		cpu_info[i].id = s_lapicIDs[i];
		cpu_info[i].currentIrql = 0;
		cpu_info[i].isBSP = false;
		cpu_info[i].schedulerTicks = 0;
		cpu_info[i].arch_specific.ist_stack = OBOS_BasicMMAllocatePages(0x20000, nullptr);
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
	Core_LowerIrql(oldIrql);
	Arch_SMPInitialized = true;
	OBOSS_UnmapPage((void*)0x1000);
	Arch_InitializeMiscFeatures();
}
_Atomic(bool) Arch_HaltCPUs = false;
_Atomic(uint8_t) Arch_CPUsHalted = 0;
bool Arch_InvlpgIPI(interrupt_frame* frame);
OBOS_NO_KASAN static void nmiHandler(interrupt_frame* frame)
{
	if (Arch_HaltCPUs)
	{
		atomic_fetch_add(&Arch_CPUsHalted, 1);
		cli();
		while (1) 
			hlt();
	}
	if (Arch_InvlpgIPI(frame))
		return;
	// if (Kdbg_Paused)
	// {
	// 	asm("sti");
	// 	// TODO: Fix 
	// 	Arch_LAPICSendEOI();
	// 	Kdbg_CallDebugExceptionHandler(frame, false);
	// 	asm("cli");
	// 	return;
	// }
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Unhandled NMI!\n");
}
OBOS_NO_KASAN static void HaltInitializedCPUs()
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
OBOS_NO_KASAN void OBOSS_HaltCPUs()
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
	return (uintptr_t)CoreS_GetCPULocalPtr()->arch_specific.ist_stack;
}

void CoreS_SetKernelStack(void* stck)
{
	if (!stck) return;
	CoreS_GetCPULocalPtr()->arch_specific.tss.rsp0 = (uintptr_t)stck+0x10000;
}
