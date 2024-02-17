/*
	oboskrnl/arch/x86_64/mm/map.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <arch/x86_64/mm/map.h>
#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/mm/pmap_l4.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/vmm_defines.h>

#include <vmm/prot.h>

#include <limine/limine.h>

#include <elf/elf64.h>

#define GetCurrentPageMap() (PageMap*)getCR3()

#define BIT(n) ((uintptr_t)1 << n)

namespace obos
{
	extern volatile limine_hhdm_request hhdm_offset;
	extern uintptr_t hhdm_limit;
	volatile limine_kernel_file_request kernel_file = {
		.id = LIMINE_KERNEL_FILE_REQUEST,
		.revision=0
	};
	namespace arch
	{
		uintptr_t DecodeProt(uintptr_t prot)
		{
			uintptr_t ret = 0;
			if (!(prot & vmm::PROT_READ_ONLY))
				ret |= BIT(1);
			if (prot & vmm::PROT_USER)
				ret |= BIT(2);
			if (prot & vmm::PROT_CACHE_DISABLE)
				ret |= BIT(4);
			if (!(prot & vmm::PROT_NO_DEMAND_PAGE))
				ret |= BIT(9);
			if (!(prot & vmm::PROT_EXECUTE))
				ret |= BIT(63);
			return ret;
		}

		void* map_page_to(PageMap* pm, uintptr_t virt, uintptr_t phys, uintptr_t prot)
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(virt))
				return nullptr;
			if (phys > ((uintptr_t)1 << GetPhysicalAddressBits()))
				return nullptr;
			virt &= ~0xfff;
			phys &= ~0xfff;
			uintptr_t flags = DecodeProt(prot) | 1;
			uintptr_t* pt = pm->AllocatePageMapAt(virt, flags);
			pt[PageMap::AddressToIndex(virt, 0)] = phys | flags;
			invlpg(virt);
			return (void*)virt;
		}
		void* map_hugepage_to(PageMap* pm, uintptr_t virt, uintptr_t phys, uintptr_t prot)
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(virt))
				return nullptr;
			if (phys & 0x1fffff)
				return nullptr;
			// Align to 2mib
			virt &= ~0x1fffff;
			uintptr_t flags = DecodeProt(prot) | 1;
			// No need to clear the flag, as it'll just get added again.
			if (flags & ((uintptr_t)1 << 7))
				flags |= ((uintptr_t)1<<12);
			uintptr_t* pt = pm->AllocatePageMapAt(virt, flags, 2);
			pt[PageMap::AddressToIndex(virt, 1)] = phys | flags | ((uintptr_t)1<<7);
			invlpg(virt);
			return (void*)virt;
		}
		void unmap(PageMap* pm, void* addr)
		{
			uintptr_t virt = (uintptr_t)addr;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(addr))
				return;
			if (!pm->GetL1PageMapEntryAt(virt))
				return;
			uintptr_t* pt = (uintptr_t*)MapToHHDM(PageMap::MaskPhysicalAddressFromEntry(pm->GetL2PageMapEntryAt(virt)));
			pt[PageMap::AddressToIndex(virt, 0)] = 0;
			pm->FreePageMapAt(virt, 3);
			invlpg(virt);
		}
		uintptr_t get_page_phys(PageMap* pm, void* addr)
		{
			return PageMap::MaskPhysicalAddressFromEntry(pm->GetL1PageMapEntryAt((uintptr_t)addr));
		}

		uintptr_t AllocatePhysicalPages(size_t nPages)
		{
			return ::obos::AllocatePhysicalPages(nPages);
		}
		void FreePhysicalPages(uintptr_t base, size_t nPages)
		{
			base &= ~(OBOS_PAGE_SIZE-1);
			::obos::FreePhysicalPages(base, nPages);
		}

		void InitializePageTables()
		{
			uintptr_t newPageMap = AllocatePhysicalPages(1);
			memzero(MapToHHDM(newPageMap), 4096);
			PageMap* pm = (PageMap*)newPageMap;
			for (uintptr_t i = hhdm_offset.response->offset; i < hhdm_limit; i += 0x200000)
				map_hugepage_to(pm, i, i - hhdm_offset.response->offset, vmm::PROT_NO_DEMAND_PAGE);
			uint8_t *kfile = (uint8_t*)kernel_file.response->kernel_file->address;
			[[maybe_unused]] size_t kfileSize = kernel_file.response->kernel_file->size;
			elf::Elf64_Ehdr* ehdr = (elf::Elf64_Ehdr*)kfile;
			elf::Elf64_Phdr* firstPhdr = (elf::Elf64_Phdr*)(kfile + ehdr->e_phoff);
			for (size_t i = 0; i < ehdr->e_phnum; i++)
			{
				if (firstPhdr[i].p_type != elf::PT_LOAD)
					continue;
				uintptr_t prot = vmm::PROT_NO_DEMAND_PAGE;
				if (firstPhdr[i].p_flags & elf::PF_X)
					prot |= vmm::PROT_EXECUTE;
				if (!(firstPhdr[i].p_flags & elf::PF_W))
					prot |= vmm::PROT_READ_ONLY;
				uint32_t nPages = firstPhdr[i].p_memsz >> 12;
				if ((firstPhdr[i].p_memsz % 4096) != 0)
					nPages++;
				uintptr_t physPages = AllocatePhysicalPages(nPages);
				uint8_t* pagesInHHDM = (uint8_t*)MapToHHDM(physPages);
				memcpy(pagesInHHDM, kfile + firstPhdr[i].p_offset, firstPhdr[i].p_filesz);
				uintptr_t base = firstPhdr[i].p_vaddr & ~0xfff;
				for (size_t j = 0; j < nPages; j++)
					map_page_to(pm, base + j * 4096, physPages + j * 4096, prot);
			}
			__asm__ __volatile__("mov %0, %%cr3" : :"r"(newPageMap) : "memory");
		}
	}
}