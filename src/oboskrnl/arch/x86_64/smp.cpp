/*
	oboskrnl/arch/x86_64/smp.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <arch/x86_64/sdt.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/irq/madt.h>
#include <arch/x86_64/irq/apic.h>

#include <arch/vmm_map.h>

#include <limine/limine.h>

#include <scheduler/cpu_local.h>

#include <vmm/init.h>
#include <vmm/map.h>
#include <irq/irql.h>

extern "C" char smp_trampoline_start[];
extern "C" char smp_trampoline_end[];
extern "C" uintptr_t smp_trampoline_cr3_loc;
extern "C" uintptr_t smp_trampoline_cpu_local_ptr;
extern "C" uintptr_t smp_trampoline_pat;
extern "C" void reload_gdt(uintptr_t gdtr);
extern "C" void enableSSE();
extern "C" void enableXSAVE();
extern "C" void enableAVX();
extern "C" void enableAVX512();

#define IA32_PAT 0x277
#define GS_BASE  0xC0000101
#define KERNEL_GS_BASE 0xC0000102

namespace obos
{
	extern volatile limine_rsdp_request rsdp_request;
	namespace scheduler
	{
		cpu_local* g_cpuInfo;
		size_t g_nCPUs;
	}
	using namespace scheduler;
	extern struct idtEntry g_idtEntries[256];
	struct idtPointer;
	extern "C" void idtFlush(idtPointer* idtr);
	namespace arch
	{
		uint8_t g_lapicIDs[256];
		static bool jumped_to_bootstrap = false;
		bool g_initializedAllCPUs = false;
		void InitializeGDTCPU(cpu_local* info)
		{
			// Initialize the TSS entry in the GDT.
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
			} tss_entry{};
			uintptr_t tss = (uintptr_t)&info->archSpecific.tss;
			tss_entry.limitLow = sizeof(info->archSpecific.tss);
			tss_entry.baseLow = tss & 0xffff;
			tss_entry.baseMiddle1 = (tss >> 16) & 0xff;
			tss_entry.baseMiddle2 = (tss >> 24) & 0xff;
			tss_entry.baseHigh = (tss >> 32) & 0xffff'ffff;
			tss_entry.access = 0x89;
			tss_entry.gran = 0x40;
			info->archSpecific.gdt[5] = *((uint64_t*)&tss_entry + 0);
			info->archSpecific.gdt[6] = *((uint64_t*)&tss_entry + 1);
			// Initialize the TSS stacks.
			info->archSpecific.tss.ist0 = info->tempStack.base + info->tempStack.size;
			info->archSpecific.tss.rsp0 = info->tempStack.base + info->tempStack.size;
			info->archSpecific.tss.iopb = sizeof(cpu_local::archSpecific.tss)-1;
			// Load the GDT and reset segment values.
			struct OBOS_PACK
			{
				uint16_t limit;
				uint64_t base;
			} gdtr{};
			gdtr.limit = sizeof(info->archSpecific.gdt) - 1;
			gdtr.base = (uintptr_t)info->archSpecific.gdt;
			reload_gdt((uintptr_t)&gdtr);
		}
		[[noreturn]] void ProcStart(cpu_local* info)
		{
			InitializeGDTCPU(info);
			// We must set GS_BASE before anything else, or we'll have problems (such as IRQL mismatching).
			wrmsr(GS_BASE, (uint64_t)info);
			wrmsr(KERNEL_GS_BASE, (uint64_t)info);
			jumped_to_bootstrap = true;
			struct
			{
				uint16_t size;
				uintptr_t idt;
			} OBOS_PACK idtPtr{ 0xfff, (uintptr_t)g_idtEntries };
			idtFlush((idtPointer*)&idtPtr);
			enableSSE();
			enableXSAVE();
			enableAVX();
			enableAVX512();
			InitializeLAPIC(g_localAPICAddress);
			info->initialized = true;
			// Enable interrupt.
			asm("sti");
			// Ensure the IRQL of the current processor is zero.
			LowerIRQL(0);
			// Hang waiting for an interrupt.
			while(1) 
				asm volatile("hlt");
		}
		size_t StartProcessors()
		{
			// Get all CPU ids.
			ACPISDTHeader* sdt = nullptr;
			size_t nEntries = 0;
			bool t32 = false;
			GetSDTFromRSDP((ACPIRSDPHeader*)rsdp_request.response->address, &sdt, &t32, &nEntries);
			char sign[4] = { 'A', 'P', 'I', 'C' };
			auto madt = (MADTTable*)GetTableWithSignature(sdt, t32, nEntries, &sign);
			size_t nCPUs = ParseMADTForLAPICIds(madt, g_lapicIDs, 0);
			ParseMADTForLAPICIds(madt, g_lapicIDs, sizeof(g_lapicIDs) / sizeof(*g_lapicIDs));
			if (nCPUs > sizeof(g_lapicIDs) / sizeof(*g_lapicIDs))
				nCPUs = sizeof(g_lapicIDs) / sizeof(*g_lapicIDs);
			g_nCPUs = nCPUs;
			g_cpuInfo = new cpu_local[g_nCPUs];
			OBOS_ASSERTP(g_cpuInfo, "Could not allocate cpu info array.");
			// Copy the trampoline to physical address zero.
			vmm::page_descriptor zeroPd{};
			zeroPd.phys = 0;
			zeroPd.virt = 0;
			zeroPd.isHugePage = false;
			zeroPd.present = true;
			zeroPd.protFlags = vmm::PROT_EXECUTE | vmm::PROT_NO_DEMAND_PAGE;
			zeroPd.awaitingDemandPagingFault = false;
			if (!vmm::MapPageDescriptor(&vmm::g_kernelContext, zeroPd))
				logger::panic(nullptr, "Could not map page descriptor at virtual address 0x%016lx, physical address 0x%016lx.\n", zeroPd.virt, zeroPd.phys);
			smp_trampoline_cr3_loc = getCR3();
			smp_trampoline_pat = rdmsr(IA32_PAT);
			memcpy(nullptr, smp_trampoline_start, smp_trampoline_end - smp_trampoline_start);
			for (size_t i = 0; i < nCPUs; i++)
			{
				uint8_t lAPIC = g_lapicIDs[i];
				g_cpuInfo[i].tempStack.size = 0x1'0000;
				g_cpuInfo[i].tempStack.base = (uintptr_t)vmm::Allocate(
					&vmm::g_kernelContext, 
					nullptr,
					g_cpuInfo[i].tempStack.size, 
					vmm::FLAGS_COMMIT | vmm::FLAGS_GUARD_PAGE_LEFT,
					vmm::PROT_NO_DEMAND_PAGE);
				g_cpuInfo[i].cpuId = lAPIC;
				if (lAPIC == g_localAPICAddress->lapicID)
				{
					g_cpuInfo[i].irql = GetIRQL();
					g_cpuInfo[i].isBSP = true;
					InitializeGDTCPU(&g_cpuInfo[i]);
					wrmsr(GS_BASE, (uint64_t)&g_cpuInfo[i]);
					wrmsr(KERNEL_GS_BASE, (uint64_t)&g_cpuInfo[i]);
					g_cpuInfo[i].initialized = true;
					continue;
				}
				g_cpuInfo[i].startupStack.size = 0x8000;
				g_cpuInfo[i].startupStack.base = (uintptr_t)vmm::Allocate(
					&vmm::g_kernelContext, 
					nullptr,
					g_cpuInfo[i].startupStack.size,
					vmm::FLAGS_COMMIT | vmm::FLAGS_GUARD_PAGE_LEFT,
					vmm::PROT_NO_DEMAND_PAGE);
				g_cpuInfo[i].initialized = false;
				g_cpuInfo[i].isBSP = false;
				*(cpu_local**)((uintptr_t)&smp_trampoline_cpu_local_ptr - (uintptr_t)&smp_trampoline_start) = &g_cpuInfo[i];
				LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::INIT, 0, lAPIC);
				LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::SIPI, 0, lAPIC);
				LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::SIPI, 0, lAPIC);
				while (!jumped_to_bootstrap);
				jumped_to_bootstrap = false;
			}
			bool allInitialized = false;
			while (!allInitialized)
			{
				allInitialized = true;
				for (size_t i = 0; i < nCPUs; i++)
					if (!g_cpuInfo[i].initialized)
						allInitialized = false;
			}
			g_initializedAllCPUs = true;
			memzero(0, 0x1000);
			vmm::Free(&vmm::g_kernelContext, nullptr, 0x1000);
			return nCPUs;
		}
		bool g_halt;
		static void StopAllInitializedCPUs(bool includingSelf)
		{
			uint8_t oldIRQL = 0;
			RaiseIRQL(0xf, &oldIRQL);
			uint32_t currentCID = GetCPUPtr() ? GetCPUPtr()->cpuId : 0;
			for (size_t cpu = 0; cpu < g_nCPUs; cpu++)
			{
				if (!g_cpuInfo[cpu].initialized)
					continue;
				if (g_cpuInfo[cpu].cpuId == currentCID)
					continue;
				LAPIC_SendIPI(DestinationShorthand::None, DeliveryMode::NMI, 0, g_cpuInfo[cpu].cpuId);
			}
			if (includingSelf)
			{
				LAPIC_SendIPI(DestinationShorthand::Self, DeliveryMode::NMI);
				while (1);
			}
			LowerIRQL(oldIRQL);
		}
		void StopCPUs(bool includingSelf)
		{
			g_halt = true;
			if (!g_initializedAllCPUs)
			{
				StopAllInitializedCPUs(includingSelf);
				return;
			}
			LAPIC_SendIPI(includingSelf ? DestinationShorthand::All : DestinationShorthand::All_Except_Self, DeliveryMode::NMI);
		}
	}
}