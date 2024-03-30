/*
	oboskrnl/arch/x86_64/entry.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <console.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/x86_64/font.h>

#include <arch/x86_64/irq/idt.h>
#include <arch/x86_64/irq/apic.h>
#include <arch/x86_64/irq/madt.h>
#include <arch/x86_64/hpet_table.h>

#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/mm/map.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/vmm_defines.h>
#include <arch/smp_cpu_func.h>

#include <vmm/init.h>
#include <vmm/prot.h>
#include <vmm/map.h>
#include <vmm/pg_context.h>

#include <allocators/slab.h>

#include <irq/irql.h>
#include <irq/irq.h>

#include <limine/limine.h>

#include <scheduler/init.h>
#include <scheduler/scheduler.h>

#include <arch/thr_context_info.h>

#include <arch/sched_timer.h>

extern "C" void InitBootGDT();
extern "C" void disablePIC();
extern "C" void enableSSE();
extern "C" void enableXSAVE();
extern "C" void enableAVX();
extern "C" void enableAVX512();

namespace obos
{
	void InitializePMM();
	void RegisterExceptionHandlers();
	volatile limine_framebuffer_request framebuffer_request = {
		.id = LIMINE_FRAMEBUFFER_REQUEST,
		.revision = 0
	};
	extern volatile limine_rsdp_request rsdp_request;
	extern volatile limine_hhdm_request hhdm_offset;
	namespace arch
	{
		size_t ThreadContextInfo::xsave_size = 0;
		HPET* g_hpetAddress;
	}
	extern void kmain();
}

using namespace obos;

LIMINE_BASE_REVISION(1);

extern "C" uint64_t calibrateHPET(uint64_t freq);

extern "C" void KernelArchInit()
{
	g_consoleFont = (void*)font_bin;
	Framebuffer initFB;
	memzero(&initFB, sizeof(initFB));
	if (framebuffer_request.response->framebuffer_count)
	{
		initFB.address = framebuffer_request.response->framebuffers[0]->address;
		initFB.bpp = framebuffer_request.response->framebuffers[0]->bpp;
		initFB.height = framebuffer_request.response->framebuffers[0]->height;
		initFB.width = framebuffer_request.response->framebuffers[0]->width;
		initFB.pitch = framebuffer_request.response->framebuffers[0]->pitch;
		initFB.format = FramebufferFormat::FB_FORMAT_RGBX8888;
	}
	new (&g_kernelConsole) Console{};
	g_kernelConsole.Initialize(&initFB, nullptr, true);
	logger::log("%s: Initializing boot GDT.\n", __func__);
	InitBootGDT();
	logger::log("%s: Initializing IDT.\n", __func__);
	InitializeIDT();
	logger::log("%s: Registering exception handlers.\n", __func__);
	RegisterExceptionHandlers();
	disablePIC();
	logger::log("%s: Initializing LAPIC.\n", __func__);
	InitializeLAPIC(GetLAPICAddress());
	logger::log("%s: Initializing IOAPIC.\n", __func__);
	uintptr_t ioapic = 0;
	ACPISDTHeader* sdt = nullptr;
	size_t nEntries = 0;
	bool t32 = false;
	GetSDTFromRSDP((ACPIRSDPHeader*)rsdp_request.response->address, &sdt, &t32, &nEntries);
	char sign[4] = { 'A', 'P', 'I', 'C' };
	auto madt = (MADTTable*)GetTableWithSignature(sdt, t32, nEntries, &sign);
	if (!madt)
		logger::panic(nullptr, "Could not find MADT table in the system.\n");
	if (ParseMADTForIOAPICAddresses(madt, &ioapic, 1))
		logger::warning("%s: There are multiple I/O APICs, but multiple I/O APICs are not supported by oboskrnl.\n", __func__);
	if (ioapic)
		InitializeIOAPIC((IOAPIC*)(hhdm_offset.response->offset + ioapic));
	else
		logger::warning("%s: Could not find an I/O APIC on this computer.\n", __func__);
	sign[0] = 'H';
	sign[1] = 'P';
	sign[2] = 'E';
	sign[3] = 'T';
	auto hpet_table = (arch::HPET_Table*)GetTableWithSignature(sdt, t32, nEntries, &sign);
	arch::g_hpetAddress = (arch::HPET*)(hhdm_offset.response->offset + hpet_table->baseAddress.address);
	uint8_t oldIRQL = 0;
	RaiseIRQL(0xf /* Mask All. */, &oldIRQL);
	asm("sti");
	logger::log("%s: Initializing PMM.\n", __func__);
	InitializePMM();
	logger::log("%s: Enabling SSE.\n", __func__);
	enableSSE();
	logger::log("%s: Enabling XSAVE (if supported).\n", __func__);
	enableXSAVE();
	logger::log("%s: Enabling AVX (if supported).\n", __func__);
	enableAVX();
	logger::log("%s: Enabling AVX-512 (if supported).\n", __func__);
	enableAVX512();
	logger::log("%s: Zeroing zero-page.\n", __func__);
	memzero(MapToHHDM(0), 4096);
	logger::log("%s: Initializing page tables.\n", __func__);
	arch::InitializePageTables();
	logger::log("%s: Initializing VMM.\n", __func__);
	vmm::InitializeVMM();
	{
		vmm::page_descriptor lapicPD{};
		lapicPD.virt = 0xffff'ffff'ffff'f000;
		lapicPD.phys = (uintptr_t)g_localAPICAddress - hhdm_offset.response->offset;
		lapicPD.protFlags = vmm::PROT_CACHE_DISABLE | vmm::PROT_NO_DEMAND_PAGE;
		lapicPD.present = true;
		lapicPD.isHugePage = false;
		vmm::MapPageDescriptor(&vmm::g_kernelContext, lapicPD);
		g_localAPICAddress = (LAPIC*)lapicPD.virt;
		vmm::page_descriptor ioapicPD{};
		ioapicPD.virt = lapicPD.virt - 0x1000;
		ioapicPD.phys = (uintptr_t)g_IOAPICAddress - hhdm_offset.response->offset;
		ioapicPD.protFlags = vmm::PROT_CACHE_DISABLE | vmm::PROT_NO_DEMAND_PAGE;
		ioapicPD.present = true;
		ioapicPD.isHugePage = false;
		vmm::MapPageDescriptor(&vmm::g_kernelContext, ioapicPD);
		g_IOAPICAddress = (IOAPIC*)ioapicPD.virt;
		vmm::page_descriptor hpetPD{};
		hpetPD.virt = ioapicPD.virt - 0x1000;
		hpetPD.phys = (uintptr_t)arch::g_hpetAddress - hhdm_offset.response->offset;
		hpetPD.protFlags = vmm::PROT_CACHE_DISABLE | vmm::PROT_NO_DEMAND_PAGE;
		hpetPD.present = true;
		hpetPD.isHugePage = false;
		vmm::MapPageDescriptor(&vmm::g_kernelContext, hpetPD);
		arch::g_hpetAddress = (arch::HPET*)hpetPD.virt;
	}
	logger::log("%s: Starting processors.\n", __func__);
	size_t nCpus = arch::StartProcessors();
	logger::debug("%s: Started %ld cores.\n", __func__, nCpus);
	logger::log("%s: Registering IPI handler\n", __func__);
	arch::RegisterIPIHandler();
	logger::log("%s: Initializing scheduler.\n", __func__);
	__cpuid__(0xd, 0, nullptr, nullptr, (uint32_t*)&arch::ThreadContextInfo::xsave_size, nullptr);
	scheduler::InitializeScheduler();
	scheduler::StartKernelMainThread(kmain);
	LowerIRQL(oldIRQL);
	// Configure watchdog timer to wait for the LAPIC timer for one second.
	// If it fails, assume something messed up and forgot to send an EOI.
	uint64_t timer = calibrateHPET(scheduler::g_schedulerFrequency/2);
	arch::g_hpetAddress->generalConfig |= 1;
	while (arch::g_hpetAddress->mainCounterValue < timer)
		pause();
	logger::warning("Watchdog timer ran out!\n");
	LAPIC_SendEOI();
	asm("sti");
	LowerIRQL(0);
	// Spin for a couple iterations.
	timer = calibrateHPET(scheduler::g_schedulerFrequency/2);
	arch::g_hpetAddress->generalConfig |= 1;
	while (arch::g_hpetAddress->mainCounterValue < timer)
		pause();
	// We're still here. Panic.
	logger::panic(nullptr, "Watchdog timer ran out while waiting for LAPIC timer for CPU %d!\n", scheduler::GetCPUPtr()->cpuId);
	// Hang waiting for an interrupt.
	while(1)
		asm volatile("hlt");
}