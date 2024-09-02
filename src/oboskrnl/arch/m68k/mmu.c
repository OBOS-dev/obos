/*
 * oboskrnl/arch/m68k/mmu.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <arch/m68k/pmm.h>
#include <arch/m68k/asm_helpers.h>

#include <arch/m68k/loader/Limine.h>

#include <mm/bare_map.h>
#include <mm/page.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <elf/elf.h>
#include <stdint.h>

extern volatile struct limine_kernel_file_request Arch_KernelFile;
extern volatile struct limine_kernel_address_request Arch_KernelAddressRequest;
extern volatile struct limine_hhdm_request Arch_HHDMRequest;

#define PT_FLAGS_RESIDENT (0b11 << 0)
#define PT_FLAGS_SUPERVISOR (0b1 << 7)
#define PT_FLAGS_READONLY (0b1 << 2)
#define PT_FLAGS_CACHE_COPYBACK (0b01 << 5)
#define PT_FLAGS_CACHE_WRITETHROUGH (0b00 << 5)
#define PT_FLAGS_CACHE_DISABLE (0b11 << 5)
#define PT_FLAGS_CACHE_DISABLE_SERALIZED (0b10 << 5)
#define PT_FLAGS_USED (1<<3)
#define PT_FLAGS_MODIFIED (1<<4)

#define MASK_PTE(pte) (((uintptr_t)(pte)) & 0xffffff00)

OBOS_NO_UBSAN OBOS_NO_KASAN obos_status Arch_MapPage(page_table pt_root, uintptr_t virt, uintptr_t to, uintptr_t ptFlags)
{
    if (!pt_root)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t pte3Index = (virt >> 25) & ((1<<7)-1);
    size_t pte2Index = (virt >> 18) & ((1<<7)-1);
    size_t pte1Index = (virt >> 12) & ((1<<6)-1);
    uintptr_t *pte3 = nullptr, *pte2 = nullptr, *pte1 = nullptr;
    ptFlags |= PT_FLAGS_RESIDENT;
    pte3 = (uintptr_t*)Arch_MapToHHDM(pt_root);
    if (!(pte3[pte3Index] & PT_FLAGS_RESIDENT))
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        uintptr_t phys = Mm_AllocatePhysicalPages(1, 1, &status);
        if (obos_is_error(status))
            return status;
        memzero(Arch_MapToHHDM(phys), OBOS_PAGE_SIZE);
        pte3[pte3Index] = ((uintptr_t)phys) | ptFlags;
    }
    else
    {
        if (!(ptFlags & PT_FLAGS_READONLY))
            pte3[pte3Index] &= ~PT_FLAGS_READONLY;
        if (!(ptFlags & PT_FLAGS_SUPERVISOR))
            pte3[pte3Index] &= ~PT_FLAGS_SUPERVISOR;
    }
    pte2 = Arch_MapToHHDM(MASK_PTE(pte3[pte3Index]));
    if (!((uintptr_t)pte2[pte2Index] & PT_FLAGS_RESIDENT))
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        uintptr_t phys = Mm_AllocatePhysicalPages(1, 1, &status);
        if (obos_is_error(status))
            return status;
        memzero(Arch_MapToHHDM(phys), OBOS_PAGE_SIZE);
        ((uintptr_t*)pte2)[pte2Index] = phys | ptFlags;
    }
    {
        if (!(ptFlags & PT_FLAGS_READONLY))
            pte2[pte2Index] &= ~PT_FLAGS_READONLY;
        if (!(ptFlags & PT_FLAGS_SUPERVISOR))
            pte2[pte2Index] &= ~PT_FLAGS_SUPERVISOR;
    }
    pte1 = (uintptr_t*)Arch_MapToHHDM(MASK_PTE(((uintptr_t*)pte2)[pte2Index]));
    pte1[pte1Index] = to | ptFlags;
    pflush(virt);
    return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status Arch_UnmapPage(page_table pt_root, uintptr_t virt)
{
    if (!pt_root)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t pte3Index = (virt >> 25) & ((1<<7)-1);
    size_t pte2Index = (virt >> 18) & ((1<<7)-1);
    size_t pte1Index = (virt >> 12) & ((1<<6)-1);
    uintptr_t *pte3 = nullptr, *pte2 = nullptr, *pte1 = nullptr;
    pte3 = (uintptr_t*)Arch_MapToHHDM(pt_root);
    if (!(pte3[pte3Index] & PT_FLAGS_RESIDENT))
            return OBOS_STATUS_NOT_FOUND;
    pte2 = Arch_MapToHHDM(MASK_PTE(pte3[pte3Index]));
    if (!((uintptr_t)pte2[pte2Index] & PT_FLAGS_RESIDENT))
        return OBOS_STATUS_NOT_FOUND;
    pte1 = (uintptr_t*)Arch_MapToHHDM(MASK_PTE(((uintptr_t*)pte2)[pte2Index]));
    if (!(pte1[pte1Index] & PT_FLAGS_RESIDENT))
        return OBOS_STATUS_NOT_FOUND;
    pte1[pte1Index] = 0;
    pflush(virt);
    if (memcmp_b(pte1, 0, 256))
    {
        pte2[pte2Index] = 0;
        Mm_FreePhysicalPages((uintptr_t)Arch_UnmapFromHHDM(pte1), 1);
        if (memcmp_b(pte2, 0, 1024))
        {
            pte3[pte3Index] = 0;
            Mm_FreePhysicalPages((uintptr_t)Arch_UnmapFromHHDM(pte2), 1);
            // Don't do this since pte3=pt_root, and we shouldn't be freeing that, or bad stuff might happen.
            // if (memcmp_b(pte3, 0, 1024))
            //     Mm_FreePhysicalPages((uintptr_t)pte3, 1);
        }
    }
    return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status Arch_GetPagePTE(page_table pt_root, uintptr_t virt, uint32_t* out)
{
    if (!out)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *out = 0;
    size_t pte3Index = (virt >> 25) & ((1<<7)-1);
    size_t pte2Index = (virt >> 18) & ((1<<7)-1);
    size_t pte1Index = (virt >> 12) & ((1<<6)-1);
    uintptr_t *pte3 = nullptr, *pte2 = nullptr, *pte1 = nullptr;
    pte3 = (uintptr_t*)Arch_MapToHHDM(pt_root);
    if (!(pte3[pte3Index] & PT_FLAGS_RESIDENT))
            return OBOS_STATUS_NOT_FOUND;
    pte2 = Arch_MapToHHDM(MASK_PTE(pte3[pte3Index]));
    if (!((uintptr_t)pte2[pte2Index] & PT_FLAGS_RESIDENT))
        return OBOS_STATUS_NOT_FOUND;
    pte1 = (uintptr_t*)Arch_MapToHHDM(MASK_PTE(((uintptr_t*)pte2)[pte2Index]));
    if (!(pte1[pte1Index] & PT_FLAGS_RESIDENT))
        return OBOS_STATUS_NOT_FOUND;
    *out = pte1[pte1Index];
    return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status OBOSS_GetPagePhysicalAddress(void* virt_, uintptr_t* oPhys)
{
    if (!oPhys)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uintptr_t virt = (uintptr_t)virt_;
    uintptr_t pt_root = 0;
    asm("movec.l %%srp, %0" :"=r"(pt_root) :);
    uintptr_t entry = 0;
    obos_status status = Arch_GetPagePTE(pt_root, virt, &entry);
    *oPhys = MASK_PTE(entry);
    return status;
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status OBOSS_MapPage_RW_XD(void* at_, uintptr_t phys)
{
    page_table cur = 0;
    asm("movec.l %%srp, %0" :"=a"(cur) :);
    return Arch_MapPage(cur, (uintptr_t)at_, phys, PT_FLAGS_SUPERVISOR|PT_FLAGS_RESIDENT|PT_FLAGS_CACHE_COPYBACK);
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status OBOSS_UnmapPage(void* at)
{
    page_table cur = 0;
    asm("movec.l %%srp, %0" :"=a"(cur) :);
    return Arch_UnmapPage(cur, (uintptr_t)at);
}
static basicmm_region kernel_region;
static basicmm_region hhdm_region;
OBOS_NO_UBSAN OBOS_NO_KASAN void Arch_InitializePageTables()
{
    page_table newPt = Mm_AllocatePhysicalPages(1, 1, nullptr);
    memzero(Arch_MapToHHDM(newPt), 4096);
    page_table oldPt = 0;
    asm("movec.l %%srp, %0" :"=a"(oldPt) :);
    // Map the HHDM.
    uintptr_t hhdm_base = Arch_HHDMRequest.response->offset;
    for (uintptr_t addr = 0; addr < Mm_PhysicalMemoryBoundaries; addr += OBOS_PAGE_SIZE)
        Arch_MapPage(newPt, hhdm_base+addr, addr, PT_FLAGS_RESIDENT|PT_FLAGS_CACHE_COPYBACK|PT_FLAGS_SUPERVISOR);
    // Map the kernel.
    Elf_Ehdr* ehdr = (Elf_Ehdr*)Arch_KernelFile.response->kernel_file->address;
    Elf_Phdr* phdrs = Arch_KernelFile.response->kernel_file->address + ehdr->e_phoff;
    // NOTE(oberrow): If the kernel shits it self, look here first.
    size_t kernelSize = 0;
    uintptr_t top = 0;
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        Elf_Phdr* phdr = &phdrs[i];
        if (phdr->p_type != PT_LOAD || !phdr->p_vaddr)
            continue;
        if (phdr->p_vaddr < Arch_KernelAddressRequest.response->virtual_base)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Bootloader made a whoopsie!\n");
        uintptr_t ptFlags = PT_FLAGS_RESIDENT;
        if ((phdr->p_flags & PF_R) && !(phdr->p_flags & PF_W))
            ptFlags |= PT_FLAGS_READONLY;
        ptFlags |= PT_FLAGS_CACHE_COPYBACK;
        ptFlags |= PT_FLAGS_SUPERVISOR;
        uintptr_t phys = 0;
        for (uintptr_t addr = phdr->p_vaddr & ~0xfff; addr < ((phdr->p_vaddr & ~0xfff) + ((phdr->p_memsz + 0xfff) & ~0xfff)); addr += 4096)
        {
            Arch_GetPagePTE(oldPt, addr, &phys);
            phys = MASK_PTE(phys);
            Arch_MapPage(newPt, addr, phys, ptFlags);
            top = addr + 0x1000;
        }
    }
    kernelSize = top-Arch_KernelAddressRequest.response->virtual_base;
    asm volatile ("movec %0, %%srp" : :"r"(newPt));
    Mm_FreePhysicalPages(oldPt, 1);
    OBOSH_BasicMMAddRegion(&kernel_region, (void*)(uintptr_t)Arch_KernelAddressRequest.response->virtual_base, kernelSize);
    OBOSH_BasicMMAddRegion(&hhdm_region, (void*)Arch_MapToHHDM(0), Mm_PhysicalMemoryBoundaries);
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status MmS_QueryPageInfo(page_table pt, uintptr_t addr, page* ppage, uintptr_t* phys)
{
    if (!pt || !ppage)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uintptr_t entry = 0;
    obos_status status = Arch_GetPagePTE(pt, addr, &entry);
    page page;
    memzero(&page, sizeof(page));
    page.prot.present = status != OBOS_STATUS_NOT_FOUND;
    if (obos_is_error(status) && status != OBOS_STATUS_NOT_FOUND)
    {
        memcpy(&ppage->prot, &page.prot, sizeof(page.prot));
        return status;
    }
    page.addr = addr;
    page.prot.huge_page = false;
    page.prot.rw = !(entry & PT_FLAGS_READONLY);
    page.prot.executable = true;
    page.prot.accessed = entry & PT_FLAGS_USED;
    page.prot.dirty = entry & PT_FLAGS_MODIFIED;
    page.prot.user = !(entry & PT_FLAGS_SUPERVISOR);
    page.prot.uc = ((entry >> 5) & 0b11) == (PT_FLAGS_CACHE_DISABLE >> 5);
    if (page.prot.accessed || page.prot.dirty)
    {
        // Unset the bit(s).
        // NOTE(oberrow): I just realized I forgot to do this on x86-64
        // Oops
        entry &= ~(PT_FLAGS_USED | PT_FLAGS_MODIFIED);
        Arch_MapPage(pt, addr, MASK_PTE(entry), entry & ~0xffffff00);
    }
    if (phys)
        *phys = MASK_PTE(entry);
    memcpy(&ppage->prot, &page.prot, sizeof(page.prot));
    return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN OBOS_NO_KASAN obos_status MmS_SetPageMapping(page_table pt, const page* page, uintptr_t phys)
{
    if (!pt || !page)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!page->prot.present)
        return Arch_UnmapPage(pt, page->addr);
    uintptr_t flags = PT_FLAGS_RESIDENT;
    if (!page->prot.rw)
        flags |= PT_FLAGS_READONLY;
    if (!page->prot.user)
        flags |= PT_FLAGS_SUPERVISOR;
    if (!page->prot.uc)
        flags |= PT_FLAGS_CACHE_COPYBACK;
    else
        flags |= PT_FLAGS_CACHE_DISABLE;
    return !page->prot.huge_page ? 
        Arch_MapPage(pt, (page->addr & ~0xfff), phys, flags) :
        OBOS_STATUS_UNIMPLEMENTED;
}
OBOS_NO_UBSAN OBOS_NO_KASAN page_table MmS_GetCurrentPageTable()
{
    page_table pt;
    asm ("movec.l %%srp, %0" :"=r"(pt) :);
    return pt;
}