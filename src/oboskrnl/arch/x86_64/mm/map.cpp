/*
	oboskrnl/arch/x86_64/mm/map.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <new>

#include <int.h>
#include <memmanip.h>
#include <fb.h>
#include <console.h>

#include <arch/x86_64/mm/map.h>
#include <arch/x86_64/mm/palloc.h>
#include <arch/x86_64/mm/pmap_l4.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/vmm_defines.h>

#include <vmm/pg_context.h>
#include <vmm/init.h>
#include <vmm/prot.h>
#include <vmm/page_node.h>

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
	volatile limine_kernel_address_request kernel_addr = {
		.id = LIMINE_KERNEL_ADDRESS_REQUEST,
		.revision=0
	};
	uintptr_t g_kernelBase = 0;
	uintptr_t g_kernelTop  = 0;
	namespace arch
	{
		uintptr_t getKernelBase()
		{
			return g_kernelBase;
		}
		uintptr_t getKernelTop()
		{
			return g_kernelTop;
		}
		uintptr_t DecodeProt(uintptr_t prot)
		{
			uintptr_t ret = 0;
			if (!(prot & vmm::PROT_NO_DEMAND_PAGE))
			{
				ret = BIT(9) | BIT(63);
				if (prot & vmm::PROT_x86_64_WRITE_COMBINING_CACHE)
					ret |= BIT(3) | BIT(7) /* Use PAT5 */;
				if (prot & vmm::PROT_x86_64_WRITE_THROUGH_CACHE)
					ret |= BIT(3) /* Use PAT1 */;
				ret |= (prot & 0x7f) << 52;
				return ret;
			}
			if (!(prot & vmm::PROT_READ_ONLY))
				ret |= BIT(1);
			if (prot & vmm::PROT_USER)
				ret |= BIT(2);
			if (prot & vmm::PROT_CACHE_DISABLE)
				ret |= BIT(4);
			if (prot & vmm::PROT_x86_64_WRITE_COMBINING_CACHE)
				ret |= BIT(3) | BIT(7) /* Use PAT5 */;
			if (prot & vmm::PROT_x86_64_WRITE_THROUGH_CACHE)
				ret |= BIT(3) /* Use PAT1 */;
			if (!(prot & vmm::PROT_EXECUTE))
				ret |= BIT(63);
			return ret;
		}
		uintptr_t DecodeEntry(uintptr_t entry)
		{
			uintptr_t ret = 0, flags = entry;
			flags &= ~((((uintptr_t)1 << GetPhysicalAddressBits()) - 1) << 12);
			if (flags & BIT(9))
			{
				ret = (flags >> 52) & 0x7f;
				if ((flags & BIT(3)) && (flags & BIT(7)) /* PAT5 */)
					ret |= vmm::PROT_x86_64_WRITE_COMBINING_CACHE;
				if ((flags & BIT(3)) /* PAT1 */)
					ret |= vmm::PROT_x86_64_WRITE_THROUGH_CACHE;
				return ret;
			}
			if (!(flags & BIT(1)))
				ret |= vmm::PROT_READ_ONLY;
			if (flags & BIT(2))
				ret |= vmm::PROT_USER;
			if (flags & BIT(4))
				ret |= vmm::PROT_CACHE_DISABLE;
			if ((flags & BIT(3)) && (flags & BIT(7)))
				ret |= vmm::PROT_x86_64_WRITE_COMBINING_CACHE;
			if ((flags & BIT(3)))
				ret |= vmm::PROT_x86_64_WRITE_THROUGH_CACHE;
			if (!(flags & BIT(63)))
				ret |= vmm::PROT_EXECUTE;
			return ret;
		}

		void* map_page_to(vmm::Context* ctx, uintptr_t virt, uintptr_t phys, vmm::prot_t prot)
		{
			return map_page_to(ctx ? ctx->GetContext()->getCR3() : GetCurrentPageMap(), virt, phys, prot);
		}
		void* map_hugepage_to(vmm::Context* ctx, uintptr_t virt, uintptr_t phys, vmm::prot_t prot)
		{
			return map_hugepage_to(ctx ? ctx->GetContext()->getCR3() : GetCurrentPageMap(), virt, phys, prot);
		}

		void unmap(vmm::Context* ctx, void* addr)
		{
			unmap(ctx ? ctx->GetContext()->getCR3() : GetCurrentPageMap(), addr);
		}

		void get_page_descriptor(vmm::Context* ctx, void* addr, vmm::page_descriptor& out)
		{
			get_page_descriptor(ctx ? ctx->GetContext()->getCR3() : GetCurrentPageMap(), addr, out);
		}

		void* map_page_to(PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot)
		{
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(virt))
				return nullptr;
			if (phys > ((uintptr_t)1 << GetPhysicalAddressBits()))
				return nullptr;
			virt &= ~0xfff;
			phys &= ~0xfff;
			uintptr_t flags = DecodeProt(prot) | 1;
			if (!(prot & vmm::PROT_NO_DEMAND_PAGE))
				flags |= (((uintptr_t)prot & 0x7f) << 52);
			uintptr_t* pt = pm->AllocatePageMapAt(virt, flags);
			pt[PageMap::AddressToIndex(virt, 0)] = phys | flags;
			if (pm == GetCurrentPageMap())
				invlpg(virt);
			return (void*)virt;
		}
		void* map_hugepage_to(PageMap* pm, uintptr_t virt, uintptr_t phys, vmm::prot_t prot)
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
			if (!(prot & vmm::PROT_NO_DEMAND_PAGE))
				flags |= (((uintptr_t)prot & 0x7f) << 52);
			uintptr_t* pt = pm->AllocatePageMapAt(virt, flags, 2);
			pt[PageMap::AddressToIndex(virt, 1)] = phys | flags | ((uintptr_t)1<<7);
			if (pm == GetCurrentPageMap())
				invlpg(virt);
			return (void*)virt;
		}
		void unmap(PageMap* pm, void* addr)
		{
			uintptr_t virt = (uintptr_t)addr;
			if (!OBOS_IS_VIRT_ADDR_CANONICAL(addr))
				return;
			uintptr_t l2Entry = pm->GetL2PageMapEntryAt(virt);
			uintptr_t l1Entry = pm->GetL1PageMapEntryAt(virt);
			bool isHugePage = l2Entry & BIT(7);
			if (!l2Entry)
				return;
			if (!l1Entry && !isHugePage)
				return;
			uintptr_t* pt = (uintptr_t*)MapToHHDM(PageMap::MaskPhysicalAddressFromEntry(isHugePage ? pm->GetL3PageMapEntryAt(virt) : l2Entry));
			pt[PageMap::AddressToIndex(virt, (uint8_t)isHugePage)] = 0;
			pm->FreePageMapAt(virt, 3 - (uint8_t)isHugePage);
			invlpg(virt);
		}
		void get_page_descriptor(class PageMap* pm, void* addr, vmm::page_descriptor& out)
		{
			out.virt = ((uintptr_t)addr) & ~0xfff;
			uintptr_t l2Entry = pm->GetL2PageMapEntryAt(out.virt);
			uintptr_t l1Entry = pm->GetL1PageMapEntryAt(out.virt);
			if (l2Entry & BIT(7))
			{
				out.isHugePage = true;
				out.present = true;
				out.awaitingDemandPagingFault = (l2Entry & BIT(9));
				out.virt = ((uintptr_t)addr) & ~0x1f'ffff;
			}
			else
			{
				out.isHugePage = false;
				out.present = (bool)(l1Entry & 1);
				out.awaitingDemandPagingFault = (l1Entry & BIT(9));
			}
			if (!out.present)
			{
				out.phys = 0;
				out.awaitingDemandPagingFault = false;
				out.protFlags = 0;
				out.isHugePage = false;
				return;
			}
			if (out.isHugePage)
			{
				bool patFlag = (l2Entry & BIT(12));
				if (!patFlag)
					l2Entry &= ~BIT(7);
				else
					l2Entry &= ~BIT(12); // Bit 7 is already set, but bit 12 being set could cause some confusion, so we clear it.
			}
			out.protFlags = DecodeEntry(out.isHugePage ? l2Entry : l1Entry);
			out.phys = PageMap::MaskPhysicalAddressFromEntry(out.isHugePage ? l2Entry : l1Entry) + ((uintptr_t)addr & (out.isHugePage ? 0x1f'ffff : 0xfff));
		}

		uintptr_t AllocatePhysicalPages(size_t nPages, bool alignToHugePageSize)
		{
			return ::obos::AllocatePhysicalPages(nPages, alignToHugePageSize);
		}
		void FreePhysicalPages(uintptr_t base, size_t nPages)
		{
			base &= ~(OBOS_PAGE_SIZE-1);
			::obos::FreePhysicalPages(base, nPages);
		}
		static void FreePageTables(uintptr_t* pm, uint8_t level, uint32_t beginIndex, uint32_t *indices)
		{
			if (!pm)
				return;
			pm = (uintptr_t*)MapToHHDM((uintptr_t)pm);
			for (indices[level] = beginIndex; indices[level] < 512; indices[level]++)
			{
				if (!pm[indices[level]])
					continue;
				if (pm[indices[level]] & BIT(7) || level == 0)
					continue;
				FreePageTables((uintptr_t*)PageMap::MaskPhysicalAddressFromEntry(pm[indices[level]]), level - 1, 0, indices);
				FreePhysicalPages(PageMap::MaskPhysicalAddressFromEntry(pm[indices[level]]), 1);
			}
		}
		static uintptr_t s_FBAddr = 0;
		static void MapFramebuffer(PageMap* pm)
		{
			Framebuffer fb{};
			g_kernelConsole.GetFramebuffer(&fb, nullptr, nullptr);
			uintptr_t fbPhys = (uintptr_t)fb.address - hhdm_offset.response->offset;
			size_t fbSize = ((size_t)fb.height * (size_t)fb.pitch);
			size_t nHugePagesInFB = fbSize / OBOS_HUGE_PAGE_SIZE;
			size_t nLeftOverPagesInFb = (fbSize - nHugePagesInFB * OBOS_HUGE_PAGE_SIZE) / OBOS_PAGE_SIZE;
			uintptr_t s_FBAddr = 0xffff'ff00'0000'0000;
			uintptr_t addr = s_FBAddr;
			s_FBAddr = s_FBAddr;
			for (; addr < (s_FBAddr + nHugePagesInFB * OBOS_HUGE_PAGE_SIZE); addr += OBOS_HUGE_PAGE_SIZE)
				map_hugepage_to(pm, addr, fbPhys + (addr - s_FBAddr), (uintptr_t)vmm::PROT_x86_64_WRITE_COMBINING_CACHE | (uintptr_t)vmm::PROT_NO_DEMAND_PAGE);
			for (; addr < (s_FBAddr + nHugePagesInFB * OBOS_HUGE_PAGE_SIZE + nLeftOverPagesInFb * OBOS_PAGE_SIZE); addr += OBOS_PAGE_SIZE)
				map_page_to(pm, addr, fbPhys + (addr - s_FBAddr), (uintptr_t)vmm::PROT_x86_64_WRITE_COMBINING_CACHE | (uintptr_t)vmm::PROT_NO_DEMAND_PAGE);
			fb.address = (void*)s_FBAddr;
			g_kernelConsole.SetFramebuffer(&fb, nullptr, true);
		}
		static void MapKernel(PageMap* pm)
		{
			uint8_t* kfile = (uint8_t*)kernel_file.response->kernel_file->address;
			[[maybe_unused]] size_t kfileSize = kernel_file.response->kernel_file->size;
			elf::Elf64_Ehdr* ehdr = (elf::Elf64_Ehdr*)kfile;
			elf::Elf64_Phdr* firstPhdr = (elf::Elf64_Phdr*)(kfile + ehdr->e_phoff);
			uintptr_t kernelBase = 0, kernelTop = 0;
			for (size_t i = 0; i < ehdr->e_phnum; i++)
			{
				if (firstPhdr[i].p_type != elf::PT_LOAD)
					continue;
				if (firstPhdr[i].p_vaddr < kernelBase || !kernelBase)
					kernelBase = firstPhdr[i].p_vaddr;
			}
			kernelTop = kernelBase;
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
				uintptr_t base = firstPhdr[i].p_vaddr & ~0xfff;
				if (((firstPhdr[i - 1].p_paddr + firstPhdr[i - 1].p_memsz) & ~0xfff) == base)
					base += 0x1000;
				kernelTop = (firstPhdr[i].p_vaddr + firstPhdr[i].p_memsz + 0xfff) & ~0xfff;
				vmm::page_descriptor pd;
				for (uintptr_t kBase = base; kBase < kernelTop; kBase += 4096)
				{
					get_page_descriptor((PageMap*)getCR3(), (void*)kBase, pd);
					map_page_to(pm, kBase, pd.phys, prot);
				}
			}
			g_kernelBase = kernelBase;
			g_kernelTop = kernelTop;
		}
		static pg_context s_internalKernelContext;
		static internal_context s_kernelCr3;
		void InitializePageTables()
		{
			uintptr_t newPageMap = obos::AllocatePhysicalPages(1);
			memzero(MapToHHDM(newPageMap), 4096);
			PageMap* pm = (PageMap*)newPageMap;
			for (uintptr_t i = hhdm_offset.response->offset; i < hhdm_limit; i += 0x200000)
				map_hugepage_to(pm, i, i - hhdm_offset.response->offset, vmm::PROT_NO_DEMAND_PAGE);
			MapKernel(pm);
			PageMap* oldPageMap = nullptr;
			__asm__ __volatile__("mov %%cr3, %0" : "=r"(oldPageMap) : : "memory");
			__asm__ __volatile__("mov %0, %%cr3" : :"r"(newPageMap) : "memory");
			// Reclaim old page tables.
			uint32_t indices[4] = {};
			FreePageTables((uintptr_t*)oldPageMap, 3, PageMap::AddressToIndex(0xffff'8000'0000'0000, 3), indices);
			FreePhysicalPages((uintptr_t)oldPageMap, 1);
			MapFramebuffer(pm);
			OptimizePMMFreeList();
			new (&s_internalKernelContext) pg_context{};
			new (&s_kernelCr3) internal_context{};
			s_kernelCr3.cr3 = pm;
			s_kernelCr3.references = 1;
			s_internalKernelContext.set(&s_kernelCr3);
			new (&vmm::g_kernelContext) vmm::Context{ &s_internalKernelContext };
		}
		void register_allocated_pages_in_context(vmm::Context* ctx)
		{
			// Register the HHDM in the context.
			vmm::page_node node{};
			node.ctx = ctx;
			node.nPageDescriptors = (hhdm_limit - hhdm_offset.response->offset) / OBOS_HUGE_PAGE_SIZE;
			node.pageDescriptors = new vmm::page_descriptor[node.nPageDescriptors];
			size_t i = 0;
			for (uintptr_t addr = hhdm_offset.response->offset; addr < hhdm_limit; addr += OBOS_HUGE_PAGE_SIZE, i++)
				get_page_descriptor(ctx, (void*)addr, node.pageDescriptors[i]);
			ctx->AppendPageNode(node);
			// NOTE(oberrow,20/04/2024): I just realized that the framebuffer is being mapped but never marked as mapped.
			// Whoopsie.
			Framebuffer fb{};
			g_kernelConsole.GetFramebuffer(&fb, nullptr, nullptr);
			uintptr_t fbPhys = (uintptr_t)fb.address - hhdm_offset.response->offset;
			size_t fbSize = ((size_t)fb.height * (size_t)fb.pitch);
			size_t nHugePagesInFB = fbSize / OBOS_HUGE_PAGE_SIZE;
			size_t nLeftOverPagesInFb = (fbSize - nHugePagesInFB * OBOS_HUGE_PAGE_SIZE) / OBOS_PAGE_SIZE;
			uintptr_t addr = s_FBAddr;
			node.nPageDescriptors = nHugePagesInFB + nLeftOverPagesInFb;
			node.pageDescriptors = new vmm::page_descriptor[node.nPageDescriptors];
			memzero(node.pageDescriptors, sizeof(vmm::page_descriptor) * node.nPageDescriptors);
			for (i = 0; addr < (s_FBAddr + nHugePagesInFB * OBOS_HUGE_PAGE_SIZE); addr += OBOS_HUGE_PAGE_SIZE, i++)
			{
				node.pageDescriptors[i].protFlags = (uintptr_t)vmm::PROT_x86_64_WRITE_COMBINING_CACHE | (uintptr_t)vmm::PROT_NO_DEMAND_PAGE;
				node.pageDescriptors[i].isHugePage = true;
				node.pageDescriptors[i].phys = fbPhys + (addr-s_FBAddr);
				node.pageDescriptors[i].virt = addr;
				node.pageDescriptors[i].present = true;
				node.pageDescriptors[i].awaitingDemandPagingFault = false;
			}
			for (; addr < (s_FBAddr + nHugePagesInFB * OBOS_HUGE_PAGE_SIZE + nLeftOverPagesInFb * OBOS_PAGE_SIZE); addr += OBOS_PAGE_SIZE, i++)
			{
				node.pageDescriptors[i].protFlags = (uintptr_t)vmm::PROT_x86_64_WRITE_COMBINING_CACHE | (uintptr_t)vmm::PROT_NO_DEMAND_PAGE;
				node.pageDescriptors[i].isHugePage = false;
				node.pageDescriptors[i].phys = fbPhys + (addr - s_FBAddr);
				node.pageDescriptors[i].virt = addr;
				node.pageDescriptors[i].present = true;
				node.pageDescriptors[i].awaitingDemandPagingFault = false;
			}
			ctx->AppendPageNode(node);
		}
	}
}