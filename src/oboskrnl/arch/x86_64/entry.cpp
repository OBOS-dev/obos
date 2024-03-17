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

#include <limine/limine.h>

#include <arch/thr_context_info.h>

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
	}
}

using namespace obos;

LIMINE_BASE_REVISION(1);

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
	}
	logger::log("%s: Starting processors.\n", __func__);
	size_t nCpus = arch::StartProcessors();
	logger::debug("%s: Started %ld cores.\n", __func__, nCpus);
	LowerIRQL(oldIRQL);
	__cpuid__(0xd, 0, nullptr, nullptr, (uint32_t*)&arch::ThreadContextInfo::xsave_size, nullptr);
	// Hang waiting for an interrupt.
	while(1) 
		asm volatile("hlt");
}