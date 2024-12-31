/*
 * oboskrnl/mm/context.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <text.h>
#include <klog.h>
#include <memmanip.h>

#include <locks/spinlock.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/swap.h>
#include <mm/pmm.h>
#include <mm/page.h>

#include <scheduler/cpu_local.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <irq/irq.h>

#if defined(__m68k__)
#	include <arch/m68k/boot_info.h>
#	include <arch/m68k/loader/Limine.h>
#endif

#define round_up(addr) (uintptr_t)((uintptr_t)(addr) + (OBOS_PAGE_SIZE - ((uintptr_t)(addr) % OBOS_PAGE_SIZE)))
#define round_up_cond(addr) ((uintptr_t)(addr) % OBOS_PAGE_SIZE ? round_up(addr) : (uintptr_t)(addr))
#define round_down(addr) (uintptr_t)((uintptr_t)(addr) - ((uintptr_t)(addr) % OBOS_PAGE_SIZE))
bool MmH_IsAddressUnPageable(uintptr_t addr)
{
	if (addr >= round_down(Core_CpuInfo) && addr < round_up_cond((uintptr_t)(Core_CpuInfo + Core_CpuCount)))
		return true;
	if (addr >= round_down(OBOS_TextRendererState.fb.base) && addr < round_up_cond((uintptr_t)OBOS_TextRendererState.fb.base + OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch))
		return true;
	if (addr >= round_down(OBOS_KernelProcess->threads.head->data) &&
		addr < round_up_cond(OBOS_KernelProcess->threads.head->data + 1))
		return true;
	if (addr >= round_down(OBOS_KernelProcess->threads.head->data->snode) &&
		addr < round_up_cond(OBOS_KernelProcess->threads.head->data->snode + 1))
		return true;
	if (addr >= round_down(Core_SchedulerIRQ) &&
		addr < round_up_cond(Core_SchedulerIRQ + 1))
		return true;
	if (addr >= round_down(OBOS_KernelProcess) &&
		addr < round_up_cond(OBOS_KernelProcess + 1))
		return true;
	for (size_t i = 0; i < Core_CpuCount; i++)
	{
		cpu_local* cpu = &Core_CpuInfo[i];
#ifdef __x86_64__
        if (addr >= round_down(cpu->idleThread) &&
			addr < round_up_cond(cpu->idleThread + 1))
			return true;
        if (addr >= round_down(cpu->idleThread->snode) &&
			addr < round_up_cond(cpu->idleThread->snode + 1))
			return true;
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up_cond((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
        if (addr >= round_down(cpu->arch_specific.ist_stack) &&
			addr < round_up_cond((uintptr_t)cpu->arch_specific.ist_stack + 0x20000))
			return true;
#elif defined(__m68k__)
		if (addr >= round_down(cpu->idleThread) &&
			addr < round_up_cond(cpu->idleThread + 1))
			return true;
        if (addr >= round_down(cpu->idleThread->snode) &&
			addr < round_up_cond(cpu->idleThread->snode + 1))
			return true;
		if (addr >= round_down(cpu->idleThread->context.stackBase) &&
			addr < round_up_cond((uintptr_t)cpu->idleThread->context.stackBase + cpu->idleThread->context.stackSize))
			return true;
extern volatile struct limine_boot_info_request Arch_BootInfo;
		if (addr >= round_down((uintptr_t)&Arch_BootInfo) && 
			addr < round_up_cond((uintptr_t)(&Arch_BootInfo + 1)))
			return true;
#else
#	error Unknown architecture.
#endif
	}
    if (!(addr >= round_down(&MmS_MMPageableRangeStart) && addr < round_up_cond(&MmS_MMPageableRangeEnd)))
		return true;
	return false;
}
RB_GENERATE_INTERNAL(page_tree, page_range, rb_node, pg_cmp_pages, OBOS_EXPORT __attribute__((optimize("-O0"))));
RB_GENERATE_INTERNAL(phys_page_tree, page, rb_node, phys_page_cmp, OBOS_EXPORT);
LIST_GENERATE(phys_page_list, struct page, lnk_node);
phys_page_tree Mm_PhysicalPages;
size_t Mm_PhysicalMemoryUsage;

page* MmH_PgAllocatePhysical(bool phys32, bool huge)
{
	size_t nPages = huge ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
	nPages /= OBOS_PAGE_SIZE;
	uintptr_t phys = phys32 ? Mm_AllocatePhysicalPages32(nPages, nPages, nullptr) : Mm_AllocatePhysicalPages(nPages, nPages, nullptr);
	return MmH_AllocatePage(phys, huge);
}

page* MmH_AllocatePage(uintptr_t phys, bool huge)
{
	page* buf = Mm_Allocator->Allocate(Mm_Allocator, sizeof(page), nullptr);
	buf->phys = phys;
	if (huge)
		buf->flags |= PHYS_PAGE_HUGE_PAGE;
	buf->pagedCount = 0;
	buf->refcount = 1;
	RB_INSERT(phys_page_tree, &Mm_PhysicalPages, buf);
	Mm_PhysicalMemoryUsage += (huge ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
	return buf;
}

void MmH_RefPage(page* buf)
{
	buf->refcount++;
}
void MmH_DerefPage(page* buf)
{
	if (!--buf->refcount)
	{
		Mm_FreePhysicalPages(buf->phys, ((buf->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE) / OBOS_PAGE_SIZE);
		RB_REMOVE(phys_page_tree, &Mm_PhysicalPages, buf);
		Mm_PhysicalMemoryUsage -= ((buf->flags & PHYS_PAGE_HUGE_PAGE) ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE);
		Mm_Allocator->Free(Mm_Allocator, buf, sizeof(*buf));
	}
}
swap_allocation_list Mm_SwapAllocations;
OBOS_NODISCARD swap_allocation* MmH_LookupSwapAllocation(uintptr_t id)
{
	swap_allocation* curr = LIST_GET_HEAD(swap_allocation_list, &Mm_SwapAllocations);
	for (; curr; )
	{
		if (curr->id == id)
			return curr;
		curr = LIST_GET_NEXT(swap_allocation_list, &Mm_SwapAllocations, curr);
	}
	return curr;
}
swap_allocation* MmH_AddSwapAllocation(uintptr_t id)
{
	swap_allocation* new = Mm_Allocator->ZeroAllocate(Mm_Allocator, 1, sizeof(swap_allocation), nullptr);
	new->id = id;
	new->provider = Mm_SwapProvider;
	new->refs = 0;
	new->phys = nullptr;
	LIST_APPEND(swap_allocation_list, &Mm_SwapAllocations, new);
	return new;
}
void MmH_RefSwapAllocation(swap_allocation* alloc)
{
	alloc->refs++;
}
void MmH_DerefSwapAllocation(swap_allocation* alloc)
{
	if (-(!alloc->refs))
	{
		LIST_REMOVE(swap_allocation_list, &Mm_SwapAllocations, alloc);
		alloc->provider->swap_free(alloc->provider, alloc->id);
		Mm_Allocator->Free(Mm_Allocator, alloc, sizeof(*alloc));
	}
}
LIST_GENERATE(swap_allocation_list, struct swap_allocation, node);

void Mm_ConstructContext(context* ctx)
{
	OBOS_ASSERT(ctx);
	memzero(ctx, sizeof(*ctx));
	ctx->pt = MmS_AllocatePageTable();
	ctx->lock = Core_SpinlockCreate();
}
