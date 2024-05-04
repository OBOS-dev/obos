/*
*	oboskrnl/arch/x86_64/entry.c
*
*	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/pmm.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x10000];
extern char Arch_InitialISTStack[0x10000];
extern void Arch_disablePIC();
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;

static thread kernelMainThread;
static thread_node kernelMainThreadNode;

static cpu_local bsp_cpu;
__asm__(
	".global Arch_IdleTask; Arch_IdleTask:; hlt; jmp Arch_IdleTask;"
);
extern void Arch_IdleTask();
extern void Arch_FlushGDT(uintptr_t gdtr);
void Arch_KernelMainBootstrap(struct ultra_boot_context* bcontext);
void Arch_KernelEntry(struct ultra_boot_context* bcontext, uint32_t magic)
{
	if (magic != ULTRA_MAGIC)
		return; // All hope is lost.
	// TODO: Parse boot context.
	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	Arch_disablePIC();
	Core_GetIrql();
	asm("sti");
	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();
	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_InitializeIDT();
	OBOS_Debug("%s: Initializing scheduler.\n", __func__);
	bsp_cpu.id = 0;
	bsp_cpu.isBSP = true;
	Core_CpuCount = 1;
	Core_CpuInfo = &bsp_cpu;
	thread_ctx ctx1, ctx2;
	memzero(&ctx1, sizeof(ctx1));
	memzero(&ctx2, sizeof(ctx2));
	CoreS_SetupThreadContext(&ctx2, Arch_KernelMainBootstrap, bcontext, false, kmain_thr_stack, 0x10000);
	CoreS_SetupThreadContext(&ctx1, Arch_IdleTask, 0, false, thr_stack, 0x4000);
	CoreH_ThreadInitialize(&kernelMainThread, THREAD_PRIORITY_NORMAL, 1, &ctx2);
	CoreH_ThreadInitialize(&bsp_idleThread, THREAD_PRIORITY_IDLE, 1, &ctx1);
	kernelMainThread.context.gs_base = &bsp_cpu;
	bsp_idleThread.context.gs_base = &bsp_cpu;
	CoreH_ThreadReadyNode(&kernelMainThread, &kernelMainThreadNode);
	CoreH_ThreadReadyNode(&bsp_idleThread, &bsp_idleThreadNode);
	// Initialize the CPU's GDT.
	Core_CpuInfo[0].arch_specific.gdtEntries[0] = 0;
	Core_CpuInfo[0].arch_specific.gdtEntries[1] = 0x00af9b000000ffff; // 64-bit code
	Core_CpuInfo[0].arch_specific.gdtEntries[2] = 0x00cf93000000ffff; // 64-bit data
	Core_CpuInfo[0].arch_specific.gdtEntries[3] = 0x00aff3000000ffff; // 64-bit user-mode data
	Core_CpuInfo[0].arch_specific.gdtEntries[4] = 0x00affb000000ffff; // 64-bit user-mode code
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
	uintptr_t tss = (uintptr_t)&Core_CpuInfo[0].arch_specific.tss;
	tss_entry.limitLow = sizeof(Core_CpuInfo[0].arch_specific.tss);
	tss_entry.baseLow = tss & 0xffff;
	tss_entry.baseMiddle1 = (tss >> 16) & 0xff;
	tss_entry.baseMiddle2 = (tss >> 24) & 0xff;
	tss_entry.baseHigh = (tss >> 32) & 0xffffffff;
	tss_entry.access = 0x89;
	tss_entry.gran = 0x40;
	Core_CpuInfo[0].arch_specific.gdtEntries[5] = *((uint64_t*)&tss_entry + 0);
	Core_CpuInfo[0].arch_specific.gdtEntries[6] = *((uint64_t*)&tss_entry + 1);

	Core_CpuInfo[0].arch_specific.tss.ist0 = Arch_InitialISTStack + sizeof(Arch_InitialISTStack);
	Core_CpuInfo[0].arch_specific.tss.rsp0 = Arch_InitialISTStack + sizeof(Arch_InitialISTStack);
	Core_CpuInfo[0].arch_specific.tss.iopb = sizeof(Core_CpuInfo[0].arch_specific.tss) - 1;
	struct
	{
		uint16_t limit;
		uintptr_t base;
	} OBOS_PACK gdtr;
	gdtr.limit = sizeof(Core_CpuInfo[0].arch_specific.gdtEntries) - 1;
	gdtr.base = Core_CpuInfo[0].arch_specific.gdtEntries;
	Arch_FlushGDT(&gdtr);
	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);
	Core_CpuInfo[0].currentIrql = Core_GetIrql();
	for (thread_priority i = 0; i <= THREAD_PRIORITY_MAX_VALUE; i++)
		Core_CpuInfo[0].priorityLists[i].priority = i;
	// Finally yield into the scheduler.
	Core_Yield();
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Scheduler did not switch to a new thread.\n");
	while (1);
}
struct ultra_memory_map_attribute* Arch_MemoryMap;
struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
struct ultra_kernel_info_attribute* Arch_KernelInfo;
const char* OBOS_KernelCmdLine;
static void ParseBootContext(struct ultra_boot_context* bcontext)
{
	struct ultra_attribute_header* header = bcontext->attributes;
	for (size_t i = 0; i < bcontext->attribute_count; i++, header = ULTRA_NEXT_ATTRIBUTE(header))
	{
		switch (header->type)
		{
		case ULTRA_ATTRIBUTE_PLATFORM_INFO: Arch_LdrPlatformInfo = header; break;
		case ULTRA_ATTRIBUTE_KERNEL_INFO: Arch_KernelInfo = header;  break;
		case ULTRA_ATTRIBUTE_MEMORY_MAP: Arch_MemoryMap = header; break;
		case ULTRA_ATTRIBUTE_MODULE_INFO: break;
		case ULTRA_ATTRIBUTE_COMMAND_LINE: OBOS_KernelCmdLine = (header + 1); break;
		case ULTRA_ATTRIBUTE_FRAMEBUFFER_INFO: break;
		case ULTRA_ATTRIBUTE_INVALID:
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid UltraProtocol attribute type %d.\n", header->type);
			break;
		default:
			break;
		}
	}
	if (!Arch_LdrPlatformInfo)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find platform info from bootloader.\n");
	if (Arch_LdrPlatformInfo->platform_type == ULTRA_PLATFORM_INVALID)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid platform type %d.\n", Arch_LdrPlatformInfo->platform_type);
	if (!Arch_KernelInfo)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find kernel info from bootloader.\n");
	if (Arch_KernelInfo->partition_type == ULTRA_PARTITION_TYPE_INVALID)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Invalid partition type %d.\n", Arch_KernelInfo->partition_type);
}
void Arch_KernelMainBootstrap(struct ultra_boot_context* bcontext)
{
	//Core_Yield();
	ParseBootContext(bcontext);
	if (Arch_LdrPlatformInfo->page_table_depth != 4)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "5-level paging is unsupported by oboskrnl.\n");
	OBOS_Debug("%s: Initializing PMM.\n", __func__);
	Arch_InitializePMM();
#define sz 0x8000ul
	OBOS_Debug("Attempt allocation of %lu bytes (%lu pages, %lu mib)\n", sz*0x1000, sz, sz/256);
	obos_status allocStatus = OBOS_STATUS_SUCCESS;
	uintptr_t addr1 = Arch_AllocatePhysicalPages(sz, 0, &allocStatus);
	if (allocStatus == OBOS_STATUS_SUCCESS)
	{
		Arch_FreePhysicalPages(addr1, sz);
		OBOS_Debug("Allocated %d pages at physical address 0x%p.\n", sz, addr1);
	}
	else
		OBOS_Debug("Allocation failed!\n");
	OBOS_Log("%s: Done early boot.\n", __func__);
	while (1);
}