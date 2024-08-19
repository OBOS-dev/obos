/*
	oboskrnl/arch/x86_64/map.c

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <stdatomic.h>

#include <mm/bare_map.h>
#include <mm/context.h>

#include <mm/pmm.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/boot_info.h>

#include <elf/elf.h>

#include <scheduler/cpu_local.h>

#include <locks/spinlock.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/lapic.h>

#include <irq/irq.h>
#include <irq/irql.h>

static OBOS_NO_KASAN size_t AddressToIndex(uintptr_t address, uint8_t level) { return (address >> (9 * level + 12)) & 0x1FF; }

OBOS_NO_KASAN uintptr_t Arch_MaskPhysicalAddressFromEntry(uintptr_t phys)
{
	return phys & 0xffffffffff000;
}
OBOS_NO_KASAN uintptr_t Arch_GetPML4Entry(uintptr_t pml4Base, uintptr_t addr)
{
	if (!pml4Base)
		return 0;
	uintptr_t* arr = (uintptr_t*)MmS_MapVirtFromPhys(Arch_MaskPhysicalAddressFromEntry(pml4Base));
	return arr[AddressToIndex(addr, 3)];
}
OBOS_NO_KASAN uintptr_t Arch_GetPML3Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML4Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)MmS_MapVirtFromPhys(phys);
	return arr[AddressToIndex(addr, 2)];
}
OBOS_NO_KASAN uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML3Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)MmS_MapVirtFromPhys(phys);
	return arr[AddressToIndex(addr, 1)];
}
OBOS_NO_KASAN uintptr_t Arch_GetPML1Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML2Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)MmS_MapVirtFromPhys(phys);
	return arr[AddressToIndex(addr, 0)];
}

static uintptr_t GetPageMapEntryForDepth(uintptr_t pml4Base, uintptr_t addr, uint8_t depth)
{
	switch (depth)
	{
	case 1:
		return Arch_GetPML2Entry(pml4Base, addr);
	case 2:
		return Arch_GetPML3Entry(pml4Base, addr);
	case 3:
		return Arch_GetPML4Entry(pml4Base, addr);
	default:
		break;
	}
	return 0;
}

uintptr_t* Arch_AllocatePageMapAt(uintptr_t pml4Base, uintptr_t at, uintptr_t cpuFlags, uint8_t depth)
{
	if (depth > 3 || depth == 0)
		return nullptr;
	cpuFlags &= ~0xfffffffff0000;
	cpuFlags |= 1;
	// Clear the caching flags.
	cpuFlags &= ~(1 << 3) & ~(1 << 4) & ~(1 << 7);
	// Clear the avaliable bits in the flags.
	cpuFlags &= ~0x07F0000000000E00;
	for (uint8_t i = 3; i > (3 - depth); i--)
	{
		uintptr_t* pageMap = (uintptr_t*)MmS_MapVirtFromPhys((i + 1) == 4 ? pml4Base : Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, i + 1)));
		if (!pageMap[AddressToIndex(at, i)])
		{
			uintptr_t newTable = Mm_AllocatePhysicalPages(1,1, nullptr);
			memzero(MmS_MapVirtFromPhys(newTable), 4096);
			pageMap[AddressToIndex(at, i)] = newTable | cpuFlags;
		}
		else
		{
			uintptr_t entry = (uintptr_t)pageMap[AddressToIndex(at, i)];
			if ((entry & ((uintptr_t)1 << 63)) && !(cpuFlags & ((uintptr_t)1 << 63)))
				entry &= ~((uintptr_t)1 << 63);
			if (!(entry & ((uintptr_t)1 << 2)) && (cpuFlags & ((uintptr_t)1 << 2)))
				entry |= ((uintptr_t)1 << 2);
			if (!(entry & ((uintptr_t)1 << 1)) && (cpuFlags & ((uintptr_t)1 << 1)))
				entry |= ((uintptr_t)1 << 1);
			pageMap[AddressToIndex(at, i)] = entry;
		}
	}
	return (uintptr_t*)MmS_MapVirtFromPhys(Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, (4 - depth))));
}
bool Arch_FreePageMapAt(uintptr_t pml4Base, uintptr_t at, uint8_t maxDepth)
{
	if (maxDepth > 3 || maxDepth == 0)
		return false;
	for (uint8_t i = (4 - maxDepth); i < 4; i++)
	{
		if (!(GetPageMapEntryForDepth(pml4Base, at, i + 1) & 1))
			continue;
		uintptr_t* pageMap = (uintptr_t*)MmS_MapVirtFromPhys(Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, i + 1)));
		uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(pageMap[AddressToIndex(at, i)]);
		uintptr_t* subPageMap = (uintptr_t*)MmS_MapVirtFromPhys(phys);
		if (memcmp_b(subPageMap, (int)0, 4096))
		{
			pageMap[AddressToIndex(at, i)] = 0;
			Mm_FreePhysicalPages(phys, 1);
			continue;
		}
	}
	return true;
}

static obos_status invlpg_impl(uintptr_t at);

obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags)
{
	if (!(((uintptr_t)(at_) >> 47) == 0 || ((uintptr_t)(at_) >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	flags |= 1;
	uintptr_t at = (uintptr_t)at_;
	if (phys & 0xfff || at & 0xfff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!(rdmsr(0xC0000080) & (1 << 11)))
		flags &= ~0x8000000000000000; // If XD is disabled in IA32_EFER (0xC0000080), disable the bit here.
	phys = Arch_MaskPhysicalAddressFromEntry(phys);
	uintptr_t* pm = Arch_AllocatePageMapAt(cr3, at, flags, 3);
	bool shouldInvplg = pm[AddressToIndex(at, 0)] & 0b1;
	pm[AddressToIndex(at, 0)] = phys | flags;
	if (shouldInvplg)
		invlpg_impl(at);
	return OBOS_STATUS_SUCCESS;
}
obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags)
{
	if (!(((uintptr_t)(at_) >> 47) == 0 || ((uintptr_t)(at_) >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	flags |= 1;
	uintptr_t at = (uintptr_t)at_;
	if (phys & 0x1fffff || at & 0x1fffff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!(rdmsr(0xC0000080) & (1 << 11)))
		flags &= ~0x8000000000000000; // If XD is disabled in IA32_EFER (0xC0000080), disable the bit here.
	if (flags & ((uintptr_t)1 << 7))
		flags |= ((uintptr_t)1 << 12);
	phys = Arch_MaskPhysicalAddressFromEntry(phys);
	uintptr_t* pm = Arch_AllocatePageMapAt(cr3, at, flags, 2);
	bool shouldInvplg = pm[AddressToIndex(at, 0)] & 0b1;
	pm[AddressToIndex(at, 1)] = phys | flags | ((uintptr_t)1 << 7);
	if (shouldInvplg)
		invlpg_impl(at);
	return OBOS_STATUS_SUCCESS;
}
static struct {
	spinlock lock;
	uintptr_t addr;
	uintptr_t cr3;
	atomic_size_t nCPUsRan;
	bool active;
	irq* irq;
} invlpg_ipi_packet;
bool Arch_InvlpgIPI(interrupt_frame* frame)
{
	OBOS_UNUSED(frame);
	if (!invlpg_ipi_packet.active)
		return false;
	if (getCR3() == invlpg_ipi_packet.cr3)
		invlpg(invlpg_ipi_packet.addr);
	invlpg_ipi_packet.nCPUsRan++;
	if (invlpg_ipi_packet.nCPUsRan == Core_CpuCount)
		invlpg_ipi_packet.active = false;
	// Arch_LAPICAddress->eoi = 0;
	return true;
}
extern bool Arch_SMPInitialized;
obos_status Arch_UnmapPage(uintptr_t cr3, void* at_)
{
	if (!(((uintptr_t)(at_) >> 47) == 0 || ((uintptr_t)(at_) >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	uintptr_t at = (uintptr_t)at_;
	if (at & 0xfff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	uintptr_t entry = Arch_GetPML2Entry(cr3, at);
	if (!(entry & (1 << 0)))
		return OBOS_STATUS_SUCCESS;
	bool isHugePage = (entry & (1ULL<<7));
	if (isHugePage)
		entry = Arch_GetPML3Entry(cr3, at);
	if (!(entry & (1<<0)))
		return OBOS_STATUS_SUCCESS;
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(entry);
	uintptr_t* pt = (uintptr_t*)MmS_MapVirtFromPhys(phys);
	pt[AddressToIndex(at, (uint8_t)isHugePage)] = 0;
	Arch_FreePageMapAt(cr3, at, 3 - (uint8_t)isHugePage);
	return invlpg_impl(at);
}
static void invlpg_ipi_bootstrap(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql) 
{
	OBOS_UNUSED(i);
	OBOS_UNUSED(userdata);
	OBOS_UNUSED(oldIrql);
	Arch_InvlpgIPI(frame);
}
static obos_status invlpg_impl(uintptr_t at)
{
	invlpg(at);
#ifndef OBOS_UP
	if (!Arch_SMPInitialized || Core_CpuCount == 1)
		return OBOS_STATUS_SUCCESS;
	if (!invlpg_ipi_packet.irq && Core_IrqInterfaceInitialized())
	{
		static irq irq;
		invlpg_ipi_packet.irq = &irq;
		enum { IRQL_INVLPG_IPI=14 };
		Core_IrqObjectInitializeIRQL(&irq, IRQL_INVLPG_IPI, false, true);
		irq.handler = invlpg_ipi_bootstrap;
		irq.handlerUserdata = nullptr;
	}
	extern bool Arch_HaltCPUs;
	while (invlpg_ipi_packet.active)
		pause();
	Arch_HaltCPUs = false;
	irql oldIrql = Core_SpinlockAcquireExplicit(&invlpg_ipi_packet.lock, IRQL_MASKED, false);
	ipi_lapic_info lapic = {
		.isShorthand=true,
		.info.shorthand = LAPIC_DESTINATION_SHORTHAND_ALL_BUT_SELF
	};
	ipi_vector_info vector = {};
	if (invlpg_ipi_packet.irq)
	{
		vector.deliveryMode = LAPIC_DELIVERY_MODE_FIXED;
		vector.info.vector = invlpg_ipi_packet.irq->vector->id+0x20;
	}
	else 
	{
		vector.deliveryMode = LAPIC_DELIVERY_MODE_NMI;
		vector.info.vector = 0;
	}
	invlpg_ipi_packet.active = true;
	invlpg_ipi_packet.addr = at;
	invlpg_ipi_packet.cr3 = getCR3();
	invlpg_ipi_packet.nCPUsRan = 1;
	obos_status status = Arch_LAPICSendIPI(lapic, vector);
	OBOS_ASSERT(obos_is_success(status));
	// This can be done async.
	// while (invlpg_ipi_packet.nCPUsRan != Core_CpuCount)
	// 	pause();
	Core_SpinlockRelease(&invlpg_ipi_packet.lock, oldIrql);
#endif
	return OBOS_STATUS_SUCCESS;
}
obos_status OBOSS_MapPage_RW_XD(void* at_, uintptr_t phys)
{
	return Arch_MapPage(getCR3(), at_, phys, 0x8000000000000003);
}
obos_status OBOSS_UnmapPage(void* at_)
{
	return Arch_UnmapPage(getCR3(), at_);
	
}
obos_status OBOSS_GetPagePhysicalAddress(void* at_, uintptr_t* oPhys)
{
	if (!(((uintptr_t)(at_) >> 47) == 0 || ((uintptr_t)(at_) >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!oPhys)
		return OBOS_STATUS_INVALID_ARGUMENT;
	uintptr_t at = (uintptr_t)at_;
	*oPhys = 0;
	uintptr_t entry = Arch_GetPML2Entry(getCR3(), at);
	if (!(entry & (1 << 0)))
		return OBOS_STATUS_SUCCESS;
	bool isHugePage = (entry & (1ULL << 7));
	if (isHugePage)
		entry = Arch_GetPML3Entry(getCR3(), at);
	if (!(entry & (1 << 0)))
		return OBOS_STATUS_SUCCESS;
	*oPhys = Arch_MaskPhysicalAddressFromEntry(((uintptr_t*)MmS_MapVirtFromPhys(Arch_MaskPhysicalAddressFromEntry(entry)))[AddressToIndex(at, (uint8_t)isHugePage)]);
	return OBOS_STATUS_SUCCESS;
}

static basicmm_region kernel_region;
static basicmm_region hhdm_region;
static void FreePageTables(uintptr_t* pm, uint8_t level, uint32_t beginIndex, uint32_t* indices)
{
	if (!pm)
		return;
	pm = (uintptr_t*)MmS_MapVirtFromPhys((uintptr_t)pm);
	for (indices[level] = beginIndex; indices[level] < 512; indices[level]++)
	{
		if (!pm[indices[level]])
			continue;
		if (pm[indices[level]] & ((uintptr_t)1<<7) || level == 0)
			continue;
		FreePageTables((uintptr_t*)Arch_MaskPhysicalAddressFromEntry(pm[indices[level]]), level - 1, 0, indices);
		Mm_FreePhysicalPages(Arch_MaskPhysicalAddressFromEntry(pm[indices[level]]), 1);
	}
}
uintptr_t MmS_KernelBaseAddress;
uintptr_t MmS_KernelEndAddress;
extern uintptr_t Arch_KernelCR3;
obos_status Arch_InitializeKernelPageTable()
{
	obos_status status = OBOS_STATUS_SUCCESS;
	uintptr_t newCR3 = Mm_AllocatePhysicalPages(1,1, &status);
	uintptr_t oldCR3 = getCR3();
	memzero(MmS_MapVirtFromPhys(newCR3), 4096);
	if (status != OBOS_STATUS_SUCCESS)
		return status;
	OBOS_Debug("%s: Mapping kernel.\n", __func__);
	Elf64_Ehdr* ehdr = (Elf64_Ehdr*)Arch_KernelBinary->address;
	Elf64_Phdr* phdrs = (Elf64_Phdr*)(Arch_KernelBinary->address + ehdr->e_phoff);
	size_t i = 0;
	for (; i < ehdr->e_phnum; i++)
	{
		Elf64_Phdr* phdr = phdrs + i;
		if (phdr->p_type != PT_LOAD)
			continue;
		if (!phdr->p_memsz)
			continue;
		uintptr_t flags = 1;
		if (!(phdr->p_flags & PF_X))
			flags |= 0x8000000000000000 /* XD */;
		if (phdr->p_flags & PF_W)
			flags |= 2;
		uintptr_t base = phdr->p_vaddr & ~0xfff;
		if (base < Arch_KernelInfo->virtual_base)
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Fatal error. Bootloader made a whoopsie! (line %d in file %s). Expression: base < Arch_KernelInfo->virtual_base.\n", __LINE__, __FILE__);
		uintptr_t limit = phdr->p_vaddr + phdr->p_memsz;
		if (limit & 0xfff)
			limit = (limit + 0xfff) & ~0xfff;
		for (uintptr_t virt = base; virt < limit; virt += OBOS_PAGE_SIZE)
		{
			uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML1Entry(oldCR3, virt));
			OBOS_ASSERT(phys);
			Arch_MapPage(newCR3, (void*)virt, phys, flags);
		}
	}
	OBOS_Debug("%s: Mapping HHDM.\n", __func__);
	for (uintptr_t off = 0; off < Mm_PhysicalMemoryBoundaries; off += 0x200000)
		Arch_MapHugePage(newCR3, MmS_MapVirtFromPhys(off), off, 0x8000000000000003 /* XD, Write, Present */);
	asm volatile("mov %0, %%cr3;" : :"r"(newCR3));
	// Reclaim old page tables.
	uint32_t indices[4] = { 0,0,0,0 };
	FreePageTables((uintptr_t*)oldCR3, 3, AddressToIndex(0xffff800000000000, 3), indices);
	Mm_FreePhysicalPages((uintptr_t)oldCR3, 1);
	OBOSH_BasicMMAddRegion(&kernel_region, (void*)Arch_KernelInfo->virtual_base, Arch_KernelInfo->size);
	OBOSH_BasicMMAddRegion(&hhdm_region, (void*)Arch_LdrPlatformInfo->higher_half_base, Mm_PhysicalMemoryBoundaries);
	Arch_KernelCR3 = newCR3;
	return OBOS_STATUS_SUCCESS;
}
obos_status MmS_QueryPageInfo(page_table pt, uintptr_t addr, page* ppage)
{
	if (!pt || !ppage)
		return OBOS_STATUS_INVALID_ARGUMENT;
	page page;
	memzero(&page, sizeof(page));
	uintptr_t pml2Entry = Arch_GetPML2Entry(pt, addr);
	uintptr_t pml1Entry = Arch_GetPML1Entry(pt, addr);
	page.prot.present = pml2Entry & BIT_TYPE(0, UL);
	if (!page.prot.present)
	{
		ppage->prot = page.prot;
		return OBOS_STATUS_SUCCESS;
	}
	page.prot.huge_page = pml2Entry & BIT_TYPE(7, UL) /* Huge page */;
	uintptr_t entry = 0;
	if (page.prot.huge_page)
	{
		addr &= ~0x1fffff;
		entry = pml2Entry;
	}
	else
	{
		addr &= ~0xfff;
		page.prot.present = pml1Entry & BIT_TYPE(0, UL);
		entry = pml1Entry;
	}
	page.addr = addr;
	page.prot.rw = entry & BIT_TYPE(1, UL);
	page.prot.user = entry & BIT_TYPE(2, UL);
	page.prot.touched = entry & (BIT_TYPE(5, UL) | BIT_TYPE(6, UL));
	page.prot.executable = !(entry & BIT_TYPE(63, UL));
	if (page.prot.huge_page)
	{
		uintptr_t pml3Entry = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML3Entry(pt, addr));
		pml3Entry = (uintptr_t)MmS_MapVirtFromPhys(pml3Entry);
		((uintptr_t*)pml3Entry)[AddressToIndex(addr, 1)] &= ~(BIT_TYPE(5, UL) | BIT_TYPE(6, UL));
	}
	else 
	{
		pml2Entry = Arch_MaskPhysicalAddressFromEntry(pml2Entry);
		pml2Entry = (uintptr_t)MmS_MapVirtFromPhys(pml2Entry);
		((uintptr_t*)pml2Entry)[AddressToIndex(addr, 0)] &= ~(BIT_TYPE(5, UL) | BIT_TYPE(6, UL));
	}
	memcpy(&ppage->prot, &page.prot, sizeof(page.prot));
	return OBOS_STATUS_SUCCESS;	
}
obos_status MmS_SetPageMapping(page_table pt, const page* page, uintptr_t phys)
{
	if (!page || !pt)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!page->prot.present)
		return Arch_UnmapPage(pt, (void*)page->addr);
	uintptr_t flags = 1;
	if (page->prot.rw)
		flags |= BIT_TYPE(1, UL);
	if (page->prot.user)
		flags |= BIT_TYPE(2, UL);
	if (!page->prot.executable)
		flags |= BIT_TYPE(63, UL);
	return !page->prot.huge_page ? 
		Arch_MapPage(pt, (void*)(page->addr & ~0xfff), phys, flags) : 
		Arch_MapHugePage(pt, (void*)(page->addr & ~0x1fffff), phys, flags);
}