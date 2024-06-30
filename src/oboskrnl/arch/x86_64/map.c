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

#include <arch/x86_64/pmm.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/boot_info.h>

#include <elf/elf64.h>

#include <mm/prot.h>
#include <mm/context.h>

#include <scheduler/cpu_local.h>

#include <locks/spinlock.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/lapic.h>

static OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM size_t AddressToIndex(uintptr_t address, uint8_t level) { return (address >> (9 * level + 12)) & 0x1FF; }

OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t Arch_MaskPhysicalAddressFromEntry(uintptr_t phys)
{
	return phys & 0xffffffffff000;
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t Arch_GetPML4Entry(uintptr_t pml4Base, uintptr_t addr)
{
	if (!pml4Base)
		return 0;
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(pml4Base));
	return arr[AddressToIndex(addr, 3)];
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t Arch_GetPML3Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML4Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(phys);
	return arr[AddressToIndex(addr, 2)];
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML3Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(phys);
	return arr[AddressToIndex(addr, 1)];
}
OBOS_NO_KASAN OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t Arch_GetPML1Entry(uintptr_t pml4Base, uintptr_t addr)
{
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(Arch_GetPML2Entry(pml4Base, addr));
	if (!phys)
		return 0;
	uintptr_t* arr = (uintptr_t*)Arch_MapToHHDM(phys);
	return arr[AddressToIndex(addr, 0)];
}

static OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t GetPageMapEntryForDepth(uintptr_t pml4Base, uintptr_t addr, uint8_t depth)
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

OBOS_EXCLUDE_FUNC_FROM_MM uintptr_t* Arch_AllocatePageMapAt(uintptr_t pml4Base, uintptr_t at, uintptr_t cpuFlags, uint8_t depth)
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
		uintptr_t* pageMap = (uintptr_t*)Arch_MapToHHDM((i + 1) == 4 ? pml4Base : Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, i + 1)));
		if (!pageMap[AddressToIndex(at, i)])
		{
			uintptr_t newTable = OBOSS_AllocatePhysicalPages(1,1, nullptr);
			memzero(Arch_MapToHHDM(newTable), 4096);
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
	return (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, (4 - depth))));
}
OBOS_EXCLUDE_FUNC_FROM_MM bool Arch_FreePageMapAt(uintptr_t pml4Base, uintptr_t at, uint8_t maxDepth)
{
	if (maxDepth > 3 || maxDepth == 0)
		return false;
	for (uint8_t i = (4 - maxDepth); i < 4; i++)
	{
		if (!(GetPageMapEntryForDepth(pml4Base, at, i + 1) & 1))
			continue;
		uintptr_t* pageMap = (uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(GetPageMapEntryForDepth(pml4Base, at, i + 1)));
		uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(pageMap[AddressToIndex(at, i)]);
		uintptr_t* subPageMap = (uintptr_t*)Arch_MapToHHDM(phys);
		if (memcmp_b(subPageMap, (int)0, 4096))
		{
			pageMap[AddressToIndex(at, i)] = 0;
			OBOSS_FreePhysicalPages(phys, 1);
			continue;
		}
	}
	return true;
}

OBOS_EXCLUDE_FUNC_FROM_MM obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags)
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
	pm[AddressToIndex(at, 0)] = phys | flags;
	return OBOS_STATUS_SUCCESS;
}
OBOS_EXCLUDE_FUNC_FROM_MM obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags)
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
	pm[AddressToIndex(at, 1)] = phys | flags | ((uintptr_t)1 << 7);
	return OBOS_STATUS_SUCCESS;
}
static OBOS_EXCLUDE_VAR_FROM_MM struct {
	spinlock lock;
	uintptr_t addr;
	uintptr_t cr3;
	atomic_size_t nCPUsRan;
	bool active;
} invlpg_ipi_packet;
OBOS_EXCLUDE_FUNC_FROM_MM bool Arch_InvlpgIPI(interrupt_frame* frame)
{
	OBOS_UNUSED(frame);
	if (!invlpg_ipi_packet.active)
		return false;
	if (getCR3() == invlpg_ipi_packet.cr3)
		invlpg(invlpg_ipi_packet.addr);
	invlpg_ipi_packet.nCPUsRan++;
	Arch_LAPICAddress->eoi = 0;
	return true;
}
extern bool Arch_SMPInitialized;
OBOS_EXCLUDE_FUNC_FROM_MM obos_status Arch_UnmapPage(uintptr_t cr3, void* at_)
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
	uintptr_t* pt = (uintptr_t*)Arch_MapToHHDM(phys);
	pt[AddressToIndex(at, (uint8_t)isHugePage)] = 0;
	Arch_FreePageMapAt(cr3, at, 3 - (uint8_t)isHugePage);
	invlpg(at);
#ifndef OBOS_UP
	if (!Arch_SMPInitialized || Core_CpuCount == 1)
		return OBOS_STATUS_SUCCESS;
	irql oldIrql = Core_SpinlockAcquire(&invlpg_ipi_packet.lock);
	ipi_lapic_info lapic = {
		.isShorthand=true,
		.info.shorthand = LAPIC_DESTINATION_SHORTHAND_ALL_BUT_SELF
	};
	ipi_vector_info vector = {
		.deliveryMode = LAPIC_DELIVERY_MODE_NMI,
	};
	invlpg_ipi_packet.active = true;
	invlpg_ipi_packet.addr = at;
	invlpg_ipi_packet.cr3 = getCR3();
	invlpg_ipi_packet.nCPUsRan = 1;
	obos_status status = Arch_LAPICSendIPI(lapic, vector);
	OBOS_ASSERT(obos_unlikely_error(status));
	while (invlpg_ipi_packet.nCPUsRan != Core_CpuCount)
		pause();
	invlpg_ipi_packet.active = false;
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
	*oPhys = Arch_MaskPhysicalAddressFromEntry(((uintptr_t*)Arch_MapToHHDM(Arch_MaskPhysicalAddressFromEntry(entry)))[AddressToIndex(at, (uint8_t)isHugePage)]);
	return OBOS_STATUS_SUCCESS;
}

static basicmm_region kernel_region;
static basicmm_region hhdm_region;
static void FreePageTables(uintptr_t* pm, uint8_t level, uint32_t beginIndex, uint32_t* indices)
{
	if (!pm)
		return;
	pm = (uintptr_t*)Arch_MapToHHDM((uintptr_t)pm);
	for (indices[level] = beginIndex; indices[level] < 512; indices[level]++)
	{
		if (!pm[indices[level]])
			continue;
		if (pm[indices[level]] & ((uintptr_t)1<<7) || level == 0)
			continue;
		FreePageTables((uintptr_t*)Arch_MaskPhysicalAddressFromEntry(pm[indices[level]]), level - 1, 0, indices);
		Arch_FreePhysicalPages(Arch_MaskPhysicalAddressFromEntry(pm[indices[level]]), 1);
	}
}
uintptr_t MmS_KernelBaseAddress;
uintptr_t MmS_KernelEndAddress;
extern uintptr_t Arch_KernelCR3;
obos_status Arch_InitializeKernelPageTable()
{
	obos_status status = OBOS_STATUS_SUCCESS;
	uintptr_t newCR3 = Arch_AllocatePhysicalPages(1,1, &status);
	uintptr_t oldCR3 = getCR3();
	memzero(Arch_MapToHHDM(newCR3), 4096);
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
	for (uintptr_t off = 0; off < Arch_PhysicalMemoryBoundaries; off += 0x200000)
		Arch_MapHugePage(newCR3, Arch_MapToHHDM(off), off, 0x8000000000000003 /* XD, Write, Present */);
	asm volatile("mov %0, %%cr3;" : :"r"(newCR3));
	// Reclaim old page tables.
	uint32_t indices[4] = { 0,0,0,0 };
	FreePageTables((uintptr_t*)oldCR3, 3, AddressToIndex(0xffff800000000000, 3), indices);
	Arch_FreePhysicalPages((uintptr_t)oldCR3, 1);
	OBOSH_BasicMMAddRegion(&kernel_region, (void*)Arch_KernelInfo->virtual_base, Arch_KernelInfo->size);
	OBOSH_BasicMMAddRegion(&hhdm_region, (void*)Arch_LdrPlatformInfo->higher_half_base, Arch_PhysicalMemoryBoundaries);
	Arch_KernelCR3 = newCR3;
	return OBOS_STATUS_SUCCESS;
}
// Both addresses are page-aligned.
extern char Arch_no_pti_start[];
extern char Arch_no_pti_end[];
obos_status MmS_PTContextInitialize(pt_context* ctx)
{
	if (!ctx)
		return OBOS_STATUS_INVALID_ARGUMENT;
	obos_status status = OBOS_STATUS_SUCCESS;
	*ctx = OBOSS_AllocatePhysicalPages(1, 1, &status);
	// Isolate kernel PTEs from those of userspace contexts.
	// We can do this fairly simply, we just need to not map anything in the kernel address space (0xffff'8000'0000'0000) or above.
	// As a side note, we must keep a couple pages mapped:
	// The base ISR handlers.
	// Any pointer to the kernel cr3.
	// The scheduler context switch function.
	// Since it's a lot easier to just use a (linker) section for this purpose, that's what we'll do.
	// Note: It is neccessary to also map IST stacks, or we will triple fault.
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		if (Core_CpuInfo[i].isBSP)
			continue;
		uintptr_t base = (uintptr_t)Core_CpuInfo[i].arch_specific.ist_stack;
		uintptr_t flags = Arch_GetPML1Entry(getCR3(), base) & ~0xffffffffff000;
		OBOS_ASSERT(flags & 0b1);
		uintptr_t phys = Arch_GetPML1Entry(getCR3(), base) & 0xffffffffff000;
		for(size_t i = 0; i < 0x10000/0x1000; i++)
			Arch_MapPage(getCR3(), (void*)(base+i*0x1000), phys+i*0x1000, flags);
	}
	for (uintptr_t addr = (uintptr_t)MmS_MMExclusionRangeStart; addr < (uintptr_t)MmS_MMExclusionRangeEnd; addr += 0x1000)
	{
		uintptr_t flags = Arch_GetPML1Entry(getCR3(), addr) & ~0xffffffffff000;
		uintptr_t phys = Arch_GetPML1Entry(getCR3(), addr) & 0xffffffffff000;
		if (!(flags & 0b1))
			continue;
		Arch_MapPage(getCR3(), (void*)addr, phys, flags);
	}
	return status;
}
OBOS_EXCLUDE_FUNC_FROM_MM obos_status MmS_PTContextMap(pt_context* ctx, uintptr_t virt, uintptr_t phys, prot_flags prot, bool present, bool isHuge)
{
	if (!ctx)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!((virt >> 47) == 0 || (virt >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	uintptr_t cr3 = *ctx;
	if (!cr3 || cr3 & 0xfff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	obos_status status = OBOS_STATUS_SUCCESS;
	uintptr_t flags = 0;
	if (!(prot & OBOS_PROTECTION_READ_ONLY))
		flags |= (1UL<<1) /* Write */;
	if (!(prot & OBOS_PROTECTION_EXECUTABLE))
		flags |= (1UL<<63) /* XD (execute disable) */;
	if (prot & OBOS_PROTECTION_USER_PAGE)
		flags |= (1UL<<2) /* User */;
	if (prot & OBOS_PROTECTION_CACHE_DISABLE)
		flags |= (1UL<<4) /* Cache Disable */;
	if (present)
	{
		if (isHuge)
			status = Arch_MapHugePage(cr3, (void*)virt, phys, flags);
		else
			status = Arch_MapPage(cr3, (void*)virt, phys, flags);
	}
	else 
	{
		status = Arch_UnmapPage(cr3, (void*)virt);
	}
	return status;
}
OBOS_EXCLUDE_FUNC_FROM_MM obos_status MmS_PTContextQueryPageInfo(const pt_context* ctx, uintptr_t virt, pt_context_page_info* info)
{
	if (!ctx || !info)
		return OBOS_STATUS_INVALID_ARGUMENT;
	if (!((virt >> 47) == 0 || (virt >> 47) == 0x1ffff))
		return OBOS_STATUS_INVALID_ARGUMENT;
	uintptr_t cr3 = *ctx;
	if (!cr3 || cr3 & 0xfff)
		return OBOS_STATUS_INVALID_ARGUMENT;
	virt &= ~0xfff;
	memzero(info, sizeof(*info));
	obos_status status = OBOS_STATUS_SUCCESS;
	uintptr_t entry = Arch_GetPML2Entry(cr3, virt);
	if (!(entry & (1 << 0)))
		return OBOS_STATUS_SUCCESS;
	bool isHugePage = (entry & (1ULL<<7));
	if (isHugePage)
		entry = Arch_GetPML3Entry(cr3, virt);
	if (!(entry & (1<<0)))
		return OBOS_STATUS_SUCCESS;
	uintptr_t phys = Arch_MaskPhysicalAddressFromEntry(entry);
	uintptr_t* pt = (uintptr_t*)Arch_MapToHHDM(phys);
	entry = pt[AddressToIndex(virt, (uint8_t)isHugePage)];
	info->huge_page = isHugePage;
	info->present = true;
	info->phys = Arch_MaskPhysicalAddressFromEntry(entry);
	info->addr = isHugePage ? virt & ~0x1fffff : virt;
	info->dirty = entry & (1UL<<6);
	info->accessed = entry & (1UL<<5);
	prot_flags prot = 0;
	uintptr_t flags = entry & ~0xffffffffff000;
	if (!(flags & BIT_TYPE(1, UL)))
		prot |= OBOS_PROTECTION_READ_ONLY;
	if (!(flags & BIT_TYPE(63, UL)))
		prot |= OBOS_PROTECTION_EXECUTABLE;
	if (flags & BIT_TYPE(2, UL))
		prot |= OBOS_PROTECTION_USER_PAGE;
	if (flags & BIT_TYPE(4, UL))
		prot |= OBOS_PROTECTION_CACHE_DISABLE;
	info->protection = prot;
	return status;
}
OBOS_EXCLUDE_FUNC_FROM_MM void* MmS_GetDM(uintptr_t phys)
{
	return Arch_MapToHHDM(phys);
}