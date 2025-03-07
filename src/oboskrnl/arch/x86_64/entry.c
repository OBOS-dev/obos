/*
 * oboskrnl/arch/x86_64/entry.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <cmdline.h>
#include <syscall.h>
#include <init_proc.h>
#include <klog.h>
#include <stdint.h>
#include <struct_packing.h>
#include <memmanip.h>
#include <text.h>
#include <font.h>
#include <partition.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>

#include <irq/irql.h>

#include <locks/spinlock.h>
#include <locks/rw_lock.h>

#include <net/tables.h>

#include <net/udp.h>
#include <net/ip.h>
#include <net/eth.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <irq/timer.h>

#include <arch/x86_64/asm_helpers.h>

#include <driver_interface/driverId.h>
#include <driver_interface/loader.h>
#include <driver_interface/header.h>
#include <driver_interface/pnp.h>

#include <allocators/basic_allocator.h>
#include <allocators/base.h>

#include <arch/x86_64/boot_info.h>

#include <arch/x86_64/lapic.h>
#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/timer.h>

#include <irq/irq.h>

#include <mm/pmm.h>
#include <mm/bare_map.h>
#include <mm/init.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/handler.h>
#include <mm/alloc.h>
#include <mm/pmm.h>
#include <mm/disk_swap.h>
#include <mm/initial_swap.h>
#include <mm/page.h>

#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>

#include <stdatomic.h>

#include <utils/tree.h>

#include <external/fixedptc.h>

#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/event.h>
#include <uacpi/context.h>
#include <uacpi/kernel_api.h>
#include <uacpi/types.h>
#include <uacpi/utilities.h>
#include <uacpi_libc.h>

#include "gdbstub/connection.h"
#include "gdbstub/gdb_udp_backend.h"

#include <vfs/init.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>
#include <vfs/fd.h>
#include <vfs/limits.h>

#include <locks/mutex.h>
#include <locks/wait.h>
#include <locks/semaphore.h>
#include <locks/event.h>

#include <power/suspend.h>
#include <power/init.h>

#include <elf/load.h>

#include <asan.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x40000];
extern char Arch_InitialISTStack[0x20000];
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;
static swap_dev swap;

static thread kernelMainThread;
static thread_node kernelMainThreadNode;
volatile struct ultra_boot_context* Arch_BootContext;

obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags, bool free_pte);

static cpu_local bsp_cpu;
extern void Arch_IdleTask();
void Arch_CPUInitializeGDT(cpu_local* info, uintptr_t istStack, size_t istStackSize);
void Arch_KernelMainBootstrap();
static void ParseBootContext(struct ultra_boot_context* bcontext);
void Arch_InstallExceptionHandlers();

struct stack_frame
{
	struct stack_frame* down;
	uintptr_t rip;
};

stack_frame OBOSS_StackFrameNext(stack_frame curr)
{
	if (!curr)
	{
		curr = __builtin_frame_address(0);
		if (curr->down)
			curr = curr->down; // use caller's stack frame, if available
		return curr;
	}
	if (!KASAN_IsAllocated((uintptr_t)&curr->down, sizeof(*curr), false))
		return nullptr;
	return curr->down;
}
uintptr_t OBOSS_StackFrameGetPC(stack_frame curr)
{
	if (!curr)
	{
		curr = __builtin_frame_address(0);
		if (curr->down)
			curr = curr->down; // use caller's stack frame, if available
	}
	if (!KASAN_IsAllocated((uintptr_t)&curr->rip, sizeof(*curr), false))
		return 0;
	return curr->rip;
}

static void e9_out(const char *str, size_t sz, void* userdata)
{
	OBOS_UNUSED(userdata);
	for (size_t i = 0; i < sz; i++)
		outb(0xe9, str[i]);
}

static const char* color_to_ansi[] = {
	"\x1b[30m",
	"\x1b[34m",
	"\x1b[32m",
	"\x1b[36m",
	"\x1b[31m",
	"\x1b[35m",
	"\x1b[38;5;52m",
	"\x1b[38;5;7m",
	"\x1b[38;5;8m",
	"\x1b[38;5;75m",
	"\x1b[38;5;10m",
	"\x1b[38;5;14m",
	"\x1b[38;5;9m",
	"\x1b[38;5;13m",
	"\x1b[38;5;11m",
	"\x1b[38;5;15m",
};
static void e9_set_color(color c, void* unused)
{
	e9_out(color_to_ansi[c], strlen(color_to_ansi[c]), unused);
}
static void e9_reset_color(void* unused)
{
	e9_out("\x1b[0m", 4, unused);
}

static void serial_out(const char *str, size_t sz, void* userdata)
{
	OBOS_UNUSED(userdata);
	for (size_t i = 0; i < sz; i++)
		outb(0x3f8, str[i]);
}

static void serial_set_color(color c, void* unused)
{
	serial_out(color_to_ansi[c], strlen(color_to_ansi[c]), unused);
}

static void serial_reset_color(void* unused)
{
	serial_out("\x1b[0m", 4, unused);
}

// Shamelessly stolen from osdev wiki.
static void init_serial_log_backend()
{
	const uint16_t PORT = 0x3f8;

	outb(PORT + 1, 0x00);    // Disable all interrupts
	outb(PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
	outb(PORT + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
	outb(PORT + 1, 0x00);    //                  (hi byte)
	outb(PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
	outb(PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
	outb(PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
	outb(PORT + 4, 0x1E);    // Set in loopback mode, test the serial chip
	outb(PORT + 0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)

	// Check if serial is faulty (i.e: not same byte as sent)
	if(inb(PORT + 0) != 0xAE)
		return;

	// If serial is not faulty set it in normal operation mode
	// (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
	outb(PORT + 4, 0x0F);

	log_backend serial_out_cb = {.write=serial_out,.set_color=serial_set_color,.reset_color=serial_reset_color};
	OBOS_AddLogSource(&serial_out_cb);
}

uintptr_t Arch_cpu_local_curr_offset;

OBOS_PAGEABLE_FUNCTION void __attribute__((no_stack_protector)) Arch_KernelEntry(struct ultra_boot_context* bcontext)
{
	Arch_cpu_local_curr_offset = offsetof(cpu_local, curr);
	bsp_cpu.id = 0;
	bsp_cpu.isBSP = true;
	Core_CpuCount = 1;
	Core_CpuInfo = &bsp_cpu;
	Core_CpuInfo->curr = Core_CpuInfo;
	Core_CpuInfo->currentIrql = IRQL_MASKED;

	extern uint64_t __stack_chk_guard;
	Core_CpuInfo->arch_specific.stack_check_guard = __stack_chk_guard;

	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);

	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	Core_GetIrql();
	ParseBootContext(bcontext);
	Arch_BootContext = bcontext;
	OBOS_ParseCMDLine();
	asm("sti");

	uint64_t log_level = OBOS_GetOPTD_Ex("log-level", LOG_LEVEL_DEBUG);
	if (log_level > 4)
		log_level = LOG_LEVEL_DEBUG;
	OBOS_SetLogLevel(log_level);
	extern uint64_t Arch_KernelCR3;
	Arch_KernelCR3 = getCR3();

#if !OBOS_ENABLE_PROFILING
	{
		uint32_t ecx = 0;
		__cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
		bool isHypervisor = ecx & BIT_TYPE(31, UL) /* Hypervisor bit: Always 0 on physical CPUs. */;
		if (isHypervisor)
		{
			log_backend e9_out_cb = {.write=e9_out,.set_color=e9_set_color,.reset_color=e9_reset_color};
			OBOS_AddLogSource(&e9_out_cb);
		}
	}
#endif

#if 0
	init_serial_log_backend();
#endif

	if (!Arch_Framebuffer)
		OBOS_Warning("No framebuffer passed by the bootloader. All kernel logs will be on port 0xE9.\n");
	else
	{
		OBOS_TextRendererState.fb.base = Arch_MapToHHDM(Arch_Framebuffer->physical_address);
		OBOS_TextRendererState.fb.bpp = Arch_Framebuffer->bpp;
		OBOS_TextRendererState.fb.format = Arch_Framebuffer->format;
		OBOS_TextRendererState.fb.height = Arch_Framebuffer->height;
		OBOS_TextRendererState.fb.width = Arch_Framebuffer->width;
		OBOS_TextRendererState.fb.pitch = Arch_Framebuffer->pitch;
		for (size_t y = 0; y < Arch_Framebuffer->height; y++)
			for (size_t x = 0; x < Arch_Framebuffer->width; x++)
				OBOS_PlotPixel(OBOS_TEXT_BACKGROUND, &((uint8_t*)OBOS_TextRendererState.fb.base)[y*Arch_Framebuffer->pitch+x*Arch_Framebuffer->bpp/8], OBOS_TextRendererState.fb.format);
		OBOS_TextRendererState.column = 0;
		OBOS_TextRendererState.row = 0;
		OBOS_TextRendererState.font = font_bin;
		OBOS_AddLogSource(&OBOS_ConsoleOutputCallback);
		if (Arch_Framebuffer->format == ULTRA_FB_FORMAT_INVALID)
			return;
	}

	OBOS_TextRendererState.fg_color = 0xffffffff;
	if (Arch_LdrPlatformInfo->page_table_depth != 4)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "5-level paging is unsupported by oboskrnl.\n");

#if OBOS_RELEASE
	OBOS_Log("Booting OBOS %s committed on %s. Build time: %s.\n", GIT_SHA1, GIT_DATE, __DATE__ " " __TIME__);
	char cpu_vendor[13] = {0};
	memset(cpu_vendor, 0, 13);
	__cpuid__(0, 0, nullptr, (uint32_t*)&cpu_vendor[0],(uint32_t*)&cpu_vendor[8], (uint32_t*)&cpu_vendor[4]);
	uint32_t ecx = 0;
	__cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
	bool isHypervisor = ecx & BIT_TYPE(31, UL) /* Hypervisor bit: Always 0 on physical CPUs. */;
	char brand_string[49];
	memset(brand_string, 0, sizeof(brand_string));
	__cpuid__(0x80000002, 0, (uint32_t*)&brand_string[0], (uint32_t*)&brand_string[4], (uint32_t*)&brand_string[8], (uint32_t*)&brand_string[12]);
	__cpuid__(0x80000003, 0, (uint32_t*)&brand_string[16], (uint32_t*)&brand_string[20], (uint32_t*)&brand_string[24], (uint32_t*)&brand_string[28]);
	__cpuid__(0x80000004, 0, (uint32_t*)&brand_string[32], (uint32_t*)&brand_string[36], (uint32_t*)&brand_string[40], (uint32_t*)&brand_string[44]);
	OBOS_Log("Running on a %s processor, cpu brand string, %s. We are currently %srunning on a hypervisor\n", cpu_vendor, brand_string, isHypervisor ? "" : "not ");
#endif

	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();

	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);

	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_InitializeIDT(true);
	Arch_InstallExceptionHandlers();

	OBOS_Debug("Enabling XD bit in IA32_EFER.\n");
	do
	{
		uint32_t edx = 0;
		__cpuid__(0x80000001, 0, nullptr, nullptr, nullptr, &edx);
		if (edx & (1 << 20))
			wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080) | (1<<11) /* XD Enable */);
	} while(0);
	do {
		uint32_t ecx = 0;
		__cpuid__(0x1, 0x0, nullptr, nullptr, &ecx, nullptr);
		if (~ecx & (1<<23))
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "OBOS requires popcnt instruction (CPUID.01H:ECX.POPCNT [Bit 23] = 0)\n");
	} while(0);

	OBOS_Debug("%s: Initializing scheduler.\n", __func__);

	thread_ctx ctx1, ctx2;
	memzero(&ctx1, sizeof(ctx1));
	memzero(&ctx2, sizeof(ctx2));
	CoreS_SetupThreadContext(&ctx2, (uintptr_t)Arch_KernelMainBootstrap, 0, false, kmain_thr_stack, 0x10000);
	CoreS_SetupThreadContext(&ctx1, (uintptr_t)Arch_IdleTask, 0, false, thr_stack, 0x4000);
	CoreH_ThreadInitialize(&kernelMainThread, THREAD_PRIORITY_NORMAL, 1, &ctx2);
	CoreH_ThreadInitialize(&bsp_idleThread, THREAD_PRIORITY_IDLE, 1, &ctx1);
	kernelMainThread.context.gs_base = (uintptr_t)&bsp_cpu;
	bsp_idleThread.context.gs_base = (uintptr_t)&bsp_cpu;
	CoreH_ThreadReadyNode(&kernelMainThread, &kernelMainThreadNode);
	CoreH_ThreadReadyNode(&bsp_idleThread, &bsp_idleThreadNode);
	Core_CpuInfo->idleThread = &bsp_idleThread;

	// Initialize the CPU's GDT.
	Arch_CPUInitializeGDT(&Core_CpuInfo[0], (uintptr_t)Arch_InitialISTStack, sizeof(Arch_InitialISTStack));
	Core_CpuInfo[0].currentIrql = Core_GetIrql();
	Core_CpuInfo[0].arch_specific.ist_stack = Arch_InitialISTStack;
	for (thread_priority i = 0; i <= THREAD_PRIORITY_MAX_VALUE; i++)
		Core_CpuInfo[0].priorityLists[i].priority = i;
	Core_CpuInfo->initialized = true;

	// Finally yield into the scheduler.
	
	Core_LowerIrql(IRQL_PASSIVE);

	Core_Yield();

	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Scheduler did not switch to a new thread.\n");
	while (1)
		asm volatile("nop" : : :);
}

volatile struct ultra_memory_map_attribute* Arch_MemoryMap;
volatile struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
volatile struct ultra_kernel_info_attribute* Arch_KernelInfo;
volatile struct ultra_module_info_attribute* Arch_KernelBinary;
volatile struct ultra_module_info_attribute* Arch_InitRDDriver;
volatile struct ultra_framebuffer* Arch_Framebuffer;

static OBOS_PAGEABLE_FUNCTION OBOS_NO_UBSAN struct ultra_module_info_attribute* FindBootModule(volatile struct ultra_boot_context* bcontext, const char* name, size_t nameLen)
{
	if (!nameLen)
		nameLen = strlen(name);
	volatile struct ultra_attribute_header* header = bcontext->attributes;
	for (size_t i = 0; i < bcontext->attribute_count; i++, header = ULTRA_NEXT_ATTRIBUTE(header))
	{
		if (header->type == ULTRA_ATTRIBUTE_MODULE_INFO)
		{
			struct ultra_module_info_attribute* module = (struct ultra_module_info_attribute*)header;
			if (uacpi_strncmp(module->name, name, nameLen) == 0)
				return module;
		}
	}
	return nullptr;
}

static OBOS_PAGEABLE_FUNCTION OBOS_NO_KASAN OBOS_NO_UBSAN void ParseBootContext(struct ultra_boot_context* bcontext)
{
	struct ultra_attribute_header* header = bcontext->attributes;
	for (size_t i = 0; i < bcontext->attribute_count; i++, header = ULTRA_NEXT_ATTRIBUTE(header))
	{
		switch (header->type)
		{
		case ULTRA_ATTRIBUTE_PLATFORM_INFO: Arch_LdrPlatformInfo = (struct ultra_platform_info_attribute*)header; break;
		case ULTRA_ATTRIBUTE_KERNEL_INFO: Arch_KernelInfo = (struct ultra_kernel_info_attribute*)header;  break;
		case ULTRA_ATTRIBUTE_MEMORY_MAP: Arch_MemoryMap = (struct ultra_memory_map_attribute*)header; break;
		case ULTRA_ATTRIBUTE_COMMAND_LINE: OBOS_KernelCmdLine = (const char*)(header + 1); break;
		case ULTRA_ATTRIBUTE_FRAMEBUFFER_INFO: 
		{
			struct ultra_framebuffer_attribute* fb = (struct ultra_framebuffer_attribute*)header;
			Arch_Framebuffer = &fb->fb;
			break;
		}
		case ULTRA_ATTRIBUTE_MODULE_INFO: 
		{
			struct ultra_module_info_attribute* module = (struct ultra_module_info_attribute*)header;
			if (strcmp(module->name, "__KERNEL__"))
				Arch_KernelBinary = module;
			break;
		}
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
	if (!Arch_KernelBinary)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find the kernel module in boot context!\nDo you set kernel-as-module to true in the hyper.cfg?\n");
}

extern obos_status Arch_InitializeKernelPageTable();

uint64_t random_number();
uint8_t random_number8();
__asm__(
	"random_number:; rdrand %rax; ret; "
	"random_number8:; rdrand %ax; mov $0, %ah; ret; "
);

allocator_info* OBOS_KernelAllocator;
static basic_allocator kalloc;
void Arch_SMPStartup();
extern bool Arch_MakeIdleTaskSleep;
static void test_allocator(allocator_info* alloc)
{
	void* to_free = nullptr;
	while (true)
	{
		size_t sz = random_number() % 4096 + 256;
		char* ret = alloc->ZeroAllocate(alloc, sz, 1, nullptr);
		ret[0] = random_number8();
		ret[sz-1] = random_number8();
		if (random_number() % 2)
		{
			alloc->Free(alloc, to_free, 0);
			to_free = ret;
		}
	}
}

void Arch_KernelMainBootstrap()
{
	//Core_Yield();
	irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
	OBOS_Debug("%s: Initializing PMM.\n", __func__);
	Mm_InitializePMM();
	OBOS_Debug("%s: Initializing page tables.\n", __func__);
	obos_status status = Arch_InitializeKernelPageTable();
	if (obos_is_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize page tables. Status: %d.\n", status);
	bsp_idleThread.context.cr3 = getCR3();
#if OBOS_ENABLE_PROFILING
	prof_start();
#endif
	OBOS_Debug("%s: Initializing allocator...\n", __func__);
	OBOSH_ConstructBasicAllocator(&kalloc);
	OBOS_KernelAllocator = (allocator_info*)&kalloc;
	{
		char* initrd_module_name = OBOS_GetOPTS("initrd-module");
		char* initrd_driver_module_name = OBOS_GetOPTS("initrd-driver-module");
		if (initrd_module_name && initrd_driver_module_name)
		{
			OBOS_Debug("InitRD module name: %s, InitRD driver name: %s.\n", initrd_module_name, initrd_driver_module_name);
			struct ultra_module_info_attribute* initrd = FindBootModule(Arch_BootContext, initrd_module_name, 0);
			Arch_InitRDDriver = FindBootModule(Arch_BootContext, initrd_driver_module_name, 0);
			if (!Arch_InitRDDriver)
				OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find module %s.\n", initrd_driver_module_name);
			if (!initrd)
				OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find module %s.\n", initrd_module_name);
			OBOS_InitrdBinary = (const char*)initrd->address;
			OBOS_InitrdSize = initrd->size;
			OBOS_Debug("InitRD is at %p (size: %d)\n", OBOS_InitrdBinary, OBOS_InitrdSize);
		}
		else
			OBOS_Warning("Could not find either 'initrd-module' or 'initrd-driver-module'. Kernel will run without an initrd.\n");
		if (initrd_module_name)
			OBOS_FreeOption(initrd_module_name);
		if (initrd_driver_module_name)
			OBOS_FreeOption(initrd_driver_module_name);
	}
	OBOS_Debug("%s: Setting up uACPI early table access\n", __func__);
	OBOS_SetupEarlyTableAccess();
	OBOS_Debug("%s: Initializing kernel process.\n", __func__);
	OBOS_KernelProcess = Core_ProcessAllocate(&status);
	if (obos_is_error(status))
	        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a process object. Status: %d.\n", status);
	OBOS_KernelProcess->pid = Core_NextPID++;
	Core_ProcessAppendThread(OBOS_KernelProcess, &kernelMainThread);
	Core_ProcessAppendThread(OBOS_KernelProcess, &bsp_idleThread);
	OBOS_Debug("%s: Initializing LAPIC.\n", __func__);
	Arch_LAPICInitialize(true);
	OBOS_Debug("%s: Initializing SMP.\n", __func__);
	Arch_SMPStartup();
	bsp_idleThread.context.gs_base = rdmsr(0xC0000101 /* GS_BASE */);
	bsp_idleThread.masterCPU = CoreS_GetCPULocalPtr();
	Core_GetCurrentThread()->masterCPU = CoreS_GetCPULocalPtr();
	OBOS_Debug("%s: Initializing IRQ interface.\n", __func__);
	if (obos_is_error(status = Core_InitializeIRQInterface()))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize irq interface. Status: %d.\n", status);
	OBOS_Debug("%s: Initializing scheduler timer.\n", __func__);
	Arch_InitializeSchedulerTimer();
	Core_LowerIrql(oldIrql);
	OBOS_Debug("%s: Initializing IOAPICs.\n", __func__);
	if (obos_is_error(status = Arch_InitializeIOAPICs()))
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not initialize I/O APICs. Status: %d\n", status);
	OBOS_Debug("%s: Initializing VMM.\n", __func__);
	Mm_InitializeInitialSwapDevice(&swap, OBOS_GetOPTD("initial-swap-size"));
	// We can reclaim the memory used.
	Mm_SwapProvider = &swap;
	Mm_Initialize();
	if (Arch_Framebuffer->physical_address)
	{
		OBOS_Debug("Mapping framebuffer as Write-Combining.\n");
		size_t size = (Arch_Framebuffer->height*Arch_Framebuffer->pitch + OBOS_HUGE_PAGE_SIZE - 1) & ~(OBOS_HUGE_PAGE_SIZE - 1);
		void* base_ = Mm_VirtualMemoryAlloc(&Mm_KernelContext, (void*)0xffffa00000000000, size, 0, VMA_FLAGS_NON_PAGED | VMA_FLAGS_HINT | VMA_FLAGS_HUGE_PAGE, nullptr, nullptr);
		uintptr_t base = (uintptr_t)base_;
		if (base)
		{
			// We got memory for the framebuffer.
			// Now modify the physical pages
			uintptr_t offset = 0;
			for (uintptr_t addr = base; addr < (base + size); addr += offset)
			{
				uintptr_t oldPhys = 0, phys = Arch_Framebuffer->physical_address + (addr-base);
				page_info info = {};
				MmS_QueryPageInfo(MmS_GetCurrentPageTable(), addr, &info, &oldPhys);
				// Present,Write,XD,Write-Combining (PAT: 0b110)
				Arch_MapHugePage(Mm_KernelContext.pt, (void*)addr, phys, BIT_TYPE(0, UL)|BIT_TYPE(1, UL)|BIT_TYPE(63, UL)|BIT_TYPE(4, UL)|BIT_TYPE(12, UL), false);
				offset = info.prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
				page what = {.phys=oldPhys};
				page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
				MmH_DerefPage(pg);
			}
		}
		OBOS_TextRendererState.fb.backbuffer_base = Mm_VirtualMemoryAlloc(
			&Mm_KernelContext, 
			(void*)(0xffffa00000000000+size), size, 
			0, VMA_FLAGS_NON_PAGED | VMA_FLAGS_HINT | VMA_FLAGS_HUGE_PAGE | VMA_FLAGS_GUARD_PAGE, 
			nullptr, nullptr);
		memcpy(OBOS_TextRendererState.fb.backbuffer_base, OBOS_TextRendererState.fb.base, OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch);
		/*for (size_t y = 0; y < Arch_Framebuffer->height; y++)
			for (size_t x = 0; x < Arch_Framebuffer->width; x++)
				// OBOS_PlotPixel(OBOS_TEXT_BACKGROUND, &((uint8_t*)OBOS_TextRendererState.fb.backbuffer_base)[y*Arch_Framebuffer->pitch+x*Arch_Framebuffer->bpp/8], OBOS_TextRendererState.fb.format);*/
		OBOS_TextRendererState.fb.base = base_;
		OBOS_TextRendererState.fb.modified_line_bitmap = OBOS_KernelAllocator->ZeroAllocate(
			OBOS_KernelAllocator,
			get_line_bitmap_size(OBOS_TextRendererState.fb.height),
			sizeof(uint32_t),
			nullptr
		);
		Mm_KernelContext.stat.committedMemory -= size*2;
		Mm_KernelContext.stat.nonPaged -= size*2;
		Mm_GlobalMemoryUsage.committedMemory -= size*2;
		Mm_GlobalMemoryUsage.nonPaged -= size*2;
	}
	OBOS_Debug("%s: Initializing timer interface.\n", __func__);
	Core_InitializeTimerInterface();
	OBOS_Debug("%s: Initializing PCI bus 0\n\n", __func__);
	Drv_EarlyPCIInitialize();
	OBOS_Log("%s: Initializing uACPI\n", __func__);
	OBOS_InitializeUACPI();
	OBOS_Debug("%s: Initializing other PCI buses\n\n", __func__);
	Drv_PCIInitialize();

	// Set the interrupt model.
	uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);

	// TODO: Unmask the IRQ where it should be unmasked (in uacpi_kernel_install_interrupt_handler)
	// Arch_IOAPICMaskIRQ(9, false);

	// allocator_info* alloc = OBOS_KernelAllocator;
	// for (size_t i = 0; i < (Core_CpuCount-1); i++)
	// {
	// 	thread* thr = CoreH_ThreadAllocate(nullptr);
	// 	thread_ctx ctx = {};
	// 	void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x10000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
	// 	CoreS_SetupThreadContext(&ctx, (uintptr_t)test_allocator, (uintptr_t)alloc, false, stack, 0x10000);
	// 	CoreH_ThreadInitialize(thr, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
	// 	thr->stackFree = CoreH_VMAStackFree;
	// 	thr->stackFreeUserdata = &Mm_KernelContext;
	// 	CoreH_ThreadReady(thr);
	// }
	// test_allocator(alloc);

	OBOS_Debug("%s: Loading kernel symbol table.\n", __func__);
	Elf64_Ehdr* ehdr = (Elf64_Ehdr*)Arch_KernelBinary->address;
	Elf64_Shdr* sectionTable = (Elf64_Shdr*)(Arch_KernelBinary->address + ehdr->e_shoff);
	if (!sectionTable)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the section table from oboskrnl.\n");
	const char* shstr_table = (const char*)(Arch_KernelBinary->address + (sectionTable + ehdr->e_shstrndx)->sh_offset);
	// Look for .symtab
	Elf64_Shdr* symtab = nullptr;
	const char* strtable = nullptr;
	for (size_t i = 0; i < ehdr->e_shnum; i++)
	{
		const char* section = shstr_table + sectionTable[i].sh_name;
		if (strcmp(section, ".symtab"))
			symtab = &sectionTable[i];
		if (strcmp(section, ".strtab"))
			strtable = (const char*)(Arch_KernelBinary->address + sectionTable[i].sh_offset);
		if (strtable && symtab)
			break;
	}
	if (!symtab)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the symbol table from oboskrnl.\n");
	Elf64_Sym* symbolTable = (Elf64_Sym*)(Arch_KernelBinary->address + symtab->sh_offset);
	for (size_t i = 0; i < symtab->sh_size/sizeof(Elf64_Sym); i++)
	{
		Elf64_Sym* esymbol = &symbolTable[i];
		int symbolType = -1;
		switch (ELF64_ST_TYPE(esymbol->st_info)) 
		{
			case STT_FUNC:
				symbolType = SYMBOL_TYPE_FUNCTION;
				break;
			case STT_FILE:
				symbolType = SYMBOL_TYPE_FILE;
				break;
			case STT_OBJECT:
				symbolType = SYMBOL_TYPE_VARIABLE;
				break;
			default:
				continue;
		}
		driver_symbol* symbol = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_symbol), nullptr);
		const char* name = strtable + esymbol->st_name;
		size_t szName = strlen(name);
		symbol->name = memcpy(OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, szName + 1, nullptr), name, szName);
		symbol->address = esymbol->st_value;
		symbol->size = esymbol->st_size;
		symbol->type = symbolType;
		switch (esymbol->st_other)
		{
			case STV_DEFAULT:
			case STV_EXPORTED:
			// since this is the kernel, everyone already gets the same object
			case STV_SINGLETON: 
				symbol->visibility = SYMBOL_VISIBILITY_DEFAULT;
				break;
			case STV_PROTECTED:
			case STV_HIDDEN:
				symbol->visibility = SYMBOL_VISIBILITY_HIDDEN;
				break;
			default:
				OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Unrecognized visibility %d.\n", esymbol->st_other);
		}
		RB_INSERT(symbol_table, &OBOS_KernelSymbolTable, symbol);
	}
	// OBOS_SetLogLevel(LOG_LEVEL_DEBUG);
	if (Arch_InitRDDriver)
	{
		OBOS_Log("Loading InitRD driver.\n");
		// Load the InitRD driver.
		status = OBOS_STATUS_SUCCESS;
		driver_id* drv = 
			Drv_LoadDriver((void*)Arch_InitRDDriver->address, Arch_InitRDDriver->size, &status);
		if (obos_is_error(status))
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not load the InitRD driver passed in module %s.\nStatus: %d.\n", Arch_InitRDDriver->name, status);
		status = Drv_StartDriver(drv, nullptr);
		if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not start the InitRD driver passed in module %s.\nStatus: %d.\nNote: This is a bug, please report it.\n", Arch_InitRDDriver->name, status);
		OBOS_Log("Loaded InitRD driver.\n");
	}
	else 
	{
		OBOS_Debug("No InitRD driver!\n");
		OBOS_Debug("Scanning command line...\n");
		char* modules_to_load = OBOS_GetOPTS("load-modules");
		if (!modules_to_load)
			OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No initrd, and no drivers passed via the command line. Further boot is impossible.\n");
		size_t len = strlen(modules_to_load);
		char* iter = modules_to_load;
		while(iter < (modules_to_load + len))
		{
			status = OBOS_STATUS_SUCCESS;
			size_t namelen = strchr(modules_to_load, ',');
			if (namelen != len)
				namelen--;
			OBOS_Debug("Loading driver %.*s.\n", namelen, iter);
			if (uacpi_strncmp(iter, "__KERNEL__", namelen) == 0)
			{
				OBOS_Error("Cannot load the kernel (__KERNEL__) as a driver.\n");
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			struct ultra_module_info_attribute* module = FindBootModule(Arch_BootContext, iter, namelen);
			if (!module)
			{
				OBOS_Warning("Could not load bootloader module %.*s. Status: %d\n", namelen, iter, OBOS_STATUS_NOT_FOUND);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			driver_id* drv = 
				Drv_LoadDriver((void*)module->address, module->size, &status);
			if (obos_is_error(status))
			{
				OBOS_Warning("Could not load driver %s. Status: %d\n", module->name, status);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			status = Drv_StartDriver(drv, nullptr);
			if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
			{
				OBOS_Warning("Could not start driver %s. Status: %d\n", module->name, status);
				status = Drv_UnloadDriver(drv);
				if (obos_is_error(status))
					OBOS_Warning("Could not unload driver %s. Status: %d\n", module->name, status);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			if (namelen != len)
				namelen++;
			iter += namelen;
		}
	}
	OBOS_Debug("%s: Initializing VFS.\n", __func__);
	Vfs_Initialize();
	// fd file1 = {}, file2 = {};
	// Vfs_FdOpen(&file1, "/test.txt", 0);
	// Vfs_FdOpen(&file2, "/test2.txt", 0);
	// char* mem1 = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, file1.vn->filesize, 0, VMA_FLAGS_PRIVATE, &file1, nullptr);
	// char* mem2 = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, file2.vn->filesize, 0, VMA_FLAGS_PRIVATE, &file2, nullptr);
	// mem2[0] = mem1[0];
	// mem1[0x1000] = mem2[0x1000];
	// OBOS_Debug("%s\n", mem2[0] == mem1[0] ? "true" : "false");
	// OBOS_Debug("%s\n", mem1[0x1000] == mem2[0x1000] ? "true" : "false");
	// Mm_VirtualMemoryFree(&Mm_KernelContext, mem1, file1.vn->filesize);
	// Mm_VirtualMemoryFree(&Mm_KernelContext, mem2, file2.vn->filesize);
	// while(1);
	OBOS_Log("%s: Loading drivers through PnP.\n", __func__);
	Drv_PnpLoadDriversAt(Vfs_Root, true);
	do {
		if (!Arch_InitRDDriver)
			break;
		char* modules_to_load = OBOS_GetOPTS("load-modules");
		if (!modules_to_load)
			break;
		size_t len = strlen(modules_to_load);
		size_t left = len;
		char* iter = modules_to_load;
		while(iter < (modules_to_load + len))
		{
			status = OBOS_STATUS_SUCCESS;
			size_t namelen = strchr(iter, ',');
			if (namelen != left)
				namelen--;
			OBOS_Debug("Loading driver %.*s.\n", namelen, iter);
			char* path = memcpy(
				OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, namelen+1, sizeof(char), nullptr),
				iter,
				namelen
			);
			fd file = {};
			status = Vfs_FdOpen(&file, path, FD_OFLAGS_READ);
			OBOS_KernelAllocator->Free(OBOS_KernelAllocator, path, namelen+1);
			if (obos_is_error(status))
			{
				OBOS_Warning("Could not load driver %*s. Status: %d\n", namelen, iter, status);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			Vfs_FdSeek(&file, 0, SEEK_END);
			size_t filesize = Vfs_FdTellOff(&file);
			Vfs_FdSeek(&file, 0, SEEK_SET);
			void *buff = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, filesize, 0, VMA_FLAGS_PRIVATE, &file, &status);
			if (obos_is_error(status))
			{
				OBOS_Warning("Could not load driver %*s. Status: %d\n", namelen, iter, status);
				Vfs_FdClose(&file);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			driver_id* drv = 
				Drv_LoadDriver(buff, filesize, &status);
			Mm_VirtualMemoryFree(&Mm_KernelContext, buff, filesize);
			Vfs_FdClose(&file);
			if (obos_is_error(status))
			{
				OBOS_Warning("Could not load driver %*s. Status: %d\n", namelen, iter, status);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			thread* main = nullptr;
			status = Drv_StartDriver(drv, &main);
			if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
			{
				OBOS_Warning("Could not start driver %*s. Status: %d\n", namelen, iter, status);
				status = Drv_UnloadDriver(drv);
				if (obos_is_error(status))
					OBOS_Warning("Could not unload driver %*s. Status: %d\n", namelen, iter, status);
				if (namelen != len)
					namelen++;
				iter += namelen;
				continue;
			}
			if (status != OBOS_STATUS_NO_ENTRY_POINT)
			{
				while ((main->flags & THREAD_FLAGS_DIED))
					OBOSS_SpinlockHint();
				if (!(--main->references) && main->free)
					main->free(main);
			}
			if (namelen != len)
				namelen++;
			iter += namelen;
			left -= namelen;
		}
	} while(0);
	OBOS_Log("%s: Probing partitions.\n", __func__);
	OBOS_PartProbeAllDrives(true);
	// uint32_t ecx = 0;
	// __cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
	// bool isHypervisor = ecx & BIT_TYPE(31, UL) /* Hypervisor bit: Always 0 on physical CPUs. */;
	// if (!isHypervisor)
	// 	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "no, just no.\n");
	OBOS_Debug("%s: Finalizing VFS initialization...\n", __func__);
	Vfs_FinalizeInitialization();

	fd com1 = {};
	Vfs_FdOpen(&com1, "/dev/COM1", FD_OFLAGS_READ|FD_OFLAGS_WRITE);

	struct {
		OBOS_ALIGNAS(8) uint8_t id;
		OBOS_ALIGNAS(8) uint32_t baudRate;
		OBOS_ALIGNAS(8) uint32_t dataBits;
		OBOS_ALIGNAS(8) uint32_t stopBits;
		OBOS_ALIGNAS(8) uint32_t parityBit;
		OBOS_ALIGNAS(8) dev_desc* connection;
	} open_serial_connection_argp = {
		.id=1,
		.baudRate=115200,
		.dataBits=3,
		.stopBits=0,
		.parityBit=0,
		.connection=&com1.desc,
	};

	Vfs_FdIoctl(&com1, 0, &open_serial_connection_argp);

	dirent* dent = VfsH_DirentLookup("/dev/r8169-eth0");
	if (dent)
	{
		Net_EthernetUp(dent->vnode);
		ip_table_entry* ent = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ip_table_entry), nullptr);
		ent->address.addr = host_to_be32(0xc0a86402);
		ent->icmp_echo_replies = true;
		ent->received_udp_packets_tree_lock = RWLOCK_INITIALIZE();
		ent->broadcast_address.addr =  host_to_be32(0xc0a864ff);
		ent->subnet_mask = 24;
		LIST_APPEND(ip_table, &dent->vnode->tables->table, ent);
		dent->vnode->tables->gateway_address.addr = host_to_be32(0xc0a86401);
		dent->vnode->tables->gateway_entry = ent;
	}

	Kdbg_InitializeHandlers();
	// TODO: Enable this through a syscall.
	// static gdb_connection gdb_conn = {};
	// Kdbg_ConnectionInitializeUDP(&gdb_conn, 1534, dent ? dent->vnode : nullptr);
	// if (OBOS_GetOPTF("enable-kdbg") && gdb_conn.pipe)
	// {
	// 	Kdbg_CurrentConnection = &gdb_conn;
	// 	OBOS_Debug("%s: Enabling KDBG.\n", __func__);
	// 	Kdbg_CurrentConnection->connection_active = true;
	// 	asm("int3");
	// }

	// OBOS_LoadInit();

	OBOS_Log("%s: Done early boot.\n", __func__);
	OBOS_Log("Currently at %ld KiB of committed memory (%ld KiB pageable), %ld KiB paged out, %ld KiB non-paged, and %ld KiB uncommitted. %ld KiB of physical memory in use. Page faulted %ld times (%ld hard, %ld soft).\n", 
		Mm_KernelContext.stat.committedMemory/0x400,
		Mm_KernelContext.stat.pageable/0x400,
		Mm_KernelContext.stat.paged/0x400,
		Mm_KernelContext.stat.nonPaged/0x400,
		Mm_KernelContext.stat.reserved/0x400,
		Mm_PhysicalMemoryUsage/0x400,
		Mm_KernelContext.stat.pageFaultCount,
		Mm_KernelContext.stat.hardPageFaultCount,
		Mm_KernelContext.stat.softPageFaultCount
    );
#if OBOS_ENABLE_PROFILING
	prof_stop();
	prof_show("oboskrnl");
#endif

	// OBOS_Log("Mm_Allocator: %d KiB in-use\n", ((basic_allocator*)Mm_Allocator)->totalMemoryAllocated/1024);
	// OBOS_Log("OBOS_KernelAllocator: %d KiB in-use\n", ((basic_allocator*)OBOS_KernelAllocator)->totalMemoryAllocated/1024);
	// OBOS_Log("OBOS_NonPagedPoolAllocator: %d KiB in-use\n", ((basic_allocator*)OBOS_NonPagedPoolAllocator)->totalMemoryAllocated/1024);
	// OBOS_Log("Vfs_Allocator: %d KiB in-use\n", ((basic_allocator*)Vfs_Allocator)->totalMemoryAllocated/1024);
	Core_ExitCurrentThread();
}
