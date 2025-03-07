/*
 * oboskrnl/arch/m68k/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <cmdline.h>
#include <text.h>
#include <klog.h>
#include <font.h>
#include <memmanip.h>

#include <arch/m68k/loader/Limine.h>

#include <allocators/base.h>
#include <allocators/basic_allocator.h>

#include <scheduler/process.h>
#include <scheduler/thread.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>

#include <arch/m68k/asm_helpers.h>
#include <arch/m68k/cpu_local_arch.h>
#include <arch/m68k/pmm.h>
#include <arch/m68k/boot_info.h>
#include <arch/m68k/goldfish_pic.h>

#include <irq/irql.h>
#include <irq/timer.h>
#include <irq/dpc.h>

#include <mm/init.h>
#include <mm/swap.h>
#include <mm/bare_map.h>
#include <mm/context.h>
#include <mm/initial_swap.h>
#include <mm/pmm.h>

#include <vfs/init.h>

#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>

#include <asan.h>

allocator_info* OBOS_KernelAllocator;
timer_frequency CoreS_TimerFrequency;

OBOS_ALIGNAS(0x10) volatile struct limine_memmap_request Arch_MemmapRequest = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};
OBOS_ALIGNAS(0x10) volatile struct limine_kernel_address_request Arch_KernelAddressRequest = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};
OBOS_ALIGNAS(0x10) volatile struct limine_hhdm_request Arch_HHDMRequest = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};
OBOS_ALIGNAS(0x10) volatile struct limine_kernel_file_request Arch_KernelFile = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
};
OBOS_ALIGNAS(0x10) volatile struct limine_boot_info_request Arch_BootInfo = {
    .id = LIMINE_BOOT_INFO_REQUEST,
    .revision = 0,
};
OBOS_ALIGNAS(0x10) volatile struct limine_module_request Arch_InitrdRequest = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};
cpu_local bsp_cpu;
thread kmain_thread;
thread idle_thread;
static thread_node kmain_node;
static thread_node idle_thread_node;
void Arch_InitializeVectorTable();
void Arch_RawRegisterInterrupt(uint8_t vec, uintptr_t f);

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

// Makeshift frame buffer lol.
// BPP=32
// Width=1024P
// Height=768P
// Pitch=4096B
// Format=XRGB8888
char Arch_Framebuffer[1024*768*4];
void Arch_KernelEntry();
static char idle_task_stack[0x10000];
static char kernel_main_stack[0x10000];
void Arch_IdleTask();
uintptr_t Arch_TTYBase = 0;
obos_status Arch_MapPage(page_table pt_root, uintptr_t virt, uintptr_t to, uintptr_t ptFlags);
void Arch_KernelEntryBootstrap()
{    
    for (uint16_t irq = 0; irq <= 255; irq++)
    {
        bsp_cpu.arch_specific.irqs[irq].irql = irq / 32 - 1;
        bsp_cpu.arch_specific.irqs[irq].nDefers = 0;
        bsp_cpu.arch_specific.irqs[irq].next = bsp_cpu.arch_specific.irqs[irq].prev = nullptr;
    }
    Core_CpuInfo = &bsp_cpu;
    Core_CpuCount = 1;
    bsp_cpu.isBSP = true;
    bsp_cpu.initialized = true;
    bsp_cpu.id = 0;
    bsp_cpu.idleThread = &idle_thread;
    for (thread_priority i = 0; i <= THREAD_PRIORITY_MAX_VALUE; i++)
		Core_CpuInfo[0].priorityLists[i].priority = i;
    irql oldIrql = Core_RaiseIrql(IRQL_MASKED);
    // OBOS_TextRendererState.fb.base = Arch_Framebuffer;
    // OBOS_TextRendererState.fb.bpp = 32;
    // OBOS_TextRendererState.fb.height = 768;
    // OBOS_TextRendererState.fb.width = 1024;
    // OBOS_TextRendererState.fb.pitch = 1024*4;
    // OBOS_TextRendererState.fb.format = OBOS_FB_FORMAT_RGBX8888;
    
    memzero(Arch_Framebuffer, 1024*768*4);
    OBOS_TextRendererState.font = font_bin;
    OBOS_KernelCmdLine = Arch_KernelFile.response->kernel_file->cmdline;
    OBOS_ParseCMDLine();
    
    uint64_t log_level = OBOS_GetOPTD_Ex("log-level", LOG_LEVEL_DEBUG);
	if (log_level > 4)
		log_level = LOG_LEVEL_DEBUG;
	OBOS_SetLogLevel(log_level);

    OBOS_Debug("Initializing Vector Base Register.\n");
    Arch_InitializeVectorTable();
    OBOS_Debug("Initializing scheduler.\n");
    Core_DefaultThreadAffinity = 1; // we will always only have one cpu
    thread_ctx ctx = {};
    memzero(&ctx, sizeof(ctx));
    CoreS_SetupThreadContext(&ctx, (uintptr_t)Arch_KernelEntry, 0, false, kernel_main_stack, 0x10000);
    CoreH_ThreadInitialize(&kmain_thread, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
    memzero(&ctx, sizeof(ctx));
    CoreS_SetupThreadContext(&ctx, (uintptr_t)Arch_IdleTask, 0, false, idle_task_stack, 0x10000);
    CoreH_ThreadInitialize(&idle_thread, THREAD_PRIORITY_IDLE, Core_DefaultThreadAffinity, &ctx);
    CoreH_ThreadReadyNode(&kmain_thread, &kmain_node);
    CoreH_ThreadReadyNode(&idle_thread, &idle_thread_node);
    Core_LowerIrql(oldIrql);
    
    // Finally, yield.
    OBOS_Debug("Yielding into kernel main thread.\n");
    Core_Yield();
}
void Arch_InitializePageTables();
obos_status Arch_MapPage(uint32_t pt_root, uintptr_t virt, uintptr_t phys, uintptr_t ptFlags);
void Arch_PageFaultHandler(interrupt_frame* frame);
static basic_allocator kalloc;
extern BootDeviceBase Arch_RTCBase;
void timer_yield(dpc* on, void* udata)
{
    OBOS_UNUSED(on);
    OBOS_UNUSED(udata);
    Arch_PICMaskIRQ(Arch_RTCBase.irq, false);
    Core_Yield();
}
void sched_timer_hnd(void* unused)
{
    static dpc sched_dpc;
    OBOS_UNUSED(unused);
    // Turns out you can't do that without breaking things [because the timer needs to be restarted]
    // Core_Yield();
    CoreH_InitializeDPC(&sched_dpc, timer_yield, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id));
}

asm (
    ".section .rodata;"
    ".global Arch_ModuleStart;"
    ".global Arch_ModuleEnd;"
    ".align 8;"
    "Arch_ModuleStart:;"
    ".incbin \"" OBOS_BINARY_DIRECTORY "/initrd\";"
    "Arch_ModuleEnd:;"
    ".section .text;"
);
extern const char Arch_ModuleStart[];
extern const char Arch_ModuleEnd[];

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
/*
typedef struct log_backend {
        void* userdata;
        void(*write)(const char* buf, size_t sz, void* userdata);
        // Can be nullptr.
        void(*set_color)(color c, void* userdata);
        // Can be nullptr if set_color is nullptr.
        void(*reset_color)(void* userdata);
} log_backend;
*/

void tty_print(const char* buf, size_t sz, void *data)
{
    OBOS_UNUSED(data);
    for (size_t i = 0; i < sz; i++)
        ((uint32_t*)Arch_TTYBase)[0] = buf[i]; // Enable device through CMD register
}
void tty_set_color(color c, void* udata)
{
    tty_print(color_to_ansi[c], strlen(color_to_ansi[c]), udata);
}
void tty_reset_color(void *udata)
{
    tty_print("\x1b[0m", 4, udata);
}
static log_backend tty_backend = { .write=tty_print, .set_color=tty_set_color, .reset_color=tty_reset_color };

void Arch_KernelEntry()
{
    Arch_RawRegisterInterrupt(0x2, (uintptr_t)Arch_PageFaultHandler);
    Arch_RawRegisterInterrupt(24, (uintptr_t)Arch_PICHandleSpurious);
    for (uint8_t vec = 25; vec < 32; vec++)
        Arch_RawRegisterInterrupt(vec, (uintptr_t)Arch_PICHandleIRQ);
    OBOS_Debug("%s: Initializing PMM.\n", __func__);
    Mm_InitializePMM();
    OBOS_Debug("%s: Initializing page tables.\n", __func__);
    Arch_InitializePageTables();
    Arch_TTYBase = Arch_BootInfo.response->uart_phys_base;
    if (Arch_TTYBase)
    {
        uintptr_t pt_root = 0;
        asm ("movec.l %%srp, %0" :"=r"(pt_root) :);
        Arch_MapPage(pt_root, 0xffffe000, Arch_TTYBase, 0b11|(1<<7)|(0b11 << 5));
        Arch_TTYBase = 0xffffe000;
        ((uint32_t*)Arch_TTYBase)[2] = 0; // disable IRQs.
        static basicmm_region tty_region = {};
        OBOSH_BasicMMAddRegion(&tty_region, (void*)Arch_TTYBase, 0x1000);
        tty_region.mmioRange = true;
        OBOS_AddLogSource(&tty_backend);
    }
    OBOS_Debug("%s: Initializing allocator.\n", __func__);
    obos_status status = OBOS_STATUS_SUCCESS;
    if (obos_is_error(status = OBOSH_ConstructBasicAllocator(&kalloc)))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize allocator. Status: %d.\n", status);
    OBOS_KernelAllocator = (allocator_info*)&kalloc;
    OBOS_Debug("%s: Initialize kernel process.\n", __func__);
    OBOS_KernelProcess = Core_ProcessAllocate(&status);
	if (obos_is_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a process object. Status: %d.\n", status);
	OBOS_KernelProcess->pid = Core_NextPID++;
	Core_ProcessAppendThread(OBOS_KernelProcess, &kmain_thread);
	Core_ProcessAppendThread(OBOS_KernelProcess, &idle_thread);
    OBOS_Debug("%s: Initializing IRQ interface.\n", __func__);
Core_InitializeIRQInterface();
    OBOS_Debug("%s: Initializing VMM.\n", __func__);
    static swap_dev swap;
    size_t swap_size = OBOS_GetOPTD("initial-swap-size");
    if (!swap_size)
        swap_size = 16*1024*1024 /* 16 MiB */;
    Mm_InitializeInitialSwapDevice(&swap, swap_size);
	Mm_SwapProvider = &swap;
	Mm_Initialize();
    OBOS_Debug("%s: Initializing timer interface.\n", __func__);
    Core_InitializeTimerInterface();
    OBOS_Debug("%s: Initializing scheduler timer.\n", __func__);
    static timer sched_timer;
    sched_timer.handler = sched_timer_hnd;
    sched_timer.userdata = nullptr;
    Core_TimerObjectInitialize(&sched_timer, TIMER_MODE_INTERVAL, 4000);
    OBOS_Debug("%s: Loading kernel symbol table.\n", __func__);
	Elf32_Ehdr* ehdr = (Elf32_Ehdr*)Arch_KernelFile.response->kernel_file->address;
	Elf32_Shdr* sectionTable = (Elf32_Shdr*)(Arch_KernelFile.response->kernel_file->address + ehdr->e_shoff);
	if (!sectionTable)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the section table from oboskrnl.\n");
	const char* shstr_table = (const char*)(Arch_KernelFile.response->kernel_file->address + (sectionTable + ehdr->e_shstrndx)->sh_offset);
	// Look for .symtab
	Elf32_Shdr* symtab = nullptr;
	const char* strtable = nullptr;
	for (size_t i = 0; i < ehdr->e_shnum; i++)
	{
		const char* section = shstr_table + sectionTable[i].sh_name;
		if (strcmp(section, ".symtab"))
			symtab = &sectionTable[i];
		if (strcmp(section, ".strtab"))
			strtable = (const char*)(Arch_KernelFile.response->kernel_file->address + sectionTable[i].sh_offset);
		if (strtable && symtab)
			break;
	}
	if (!symtab)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the symbol table from oboskrnl.\n");
	Elf32_Sym* symbolTable = (Elf32_Sym*)(Arch_KernelFile.response->kernel_file->address + symtab->sh_offset);
	for (size_t i = 0; i < symtab->sh_size/sizeof(Elf32_Sym); i++)
	{
		Elf32_Sym* esymbol = &symbolTable[i];
		int symbolType = -1;
		switch (ELF32_ST_TYPE(esymbol->st_info)) 
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
		driver_symbol* symbol = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_symbol), nullptr);
		const char* name = strtable + esymbol->st_name;
		size_t szName = strlen(name);
		symbol->name = memcpy(ZeroAllocate(OBOS_KernelAllocator, 1, szName + 1, nullptr), name, szName);
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
    OBOS_Debug("%s: Loading InitRD driver.\n", __func__);
    status = OBOS_STATUS_SUCCESS;
    const void* file = Arch_ModuleStart;
    size_t filesize = (uintptr_t)Arch_ModuleEnd-(uintptr_t)Arch_ModuleStart;
    driver_id* id = 
        Drv_LoadDriver(file, filesize, &status);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not load driver! Status: %d.\n", status);
    Drv_StartDriver(id, nullptr);
    OBOS_Debug("%s: Initializing VFS.\n", __func__);
    OBOS_InitrdBinary = Arch_InitrdRequest.response->modules[0]->address;
    OBOS_InitrdSize = Arch_InitrdRequest.response->modules[0]->size;
    Vfs_Initialize();
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
    Core_ExitCurrentThread();
}

void Arch_IdleTask()
{
    while(1);
}

cpu_local* CoreS_GetCPULocalPtr()
{
    return &bsp_cpu;
}

BootInfoTag* Arch_GetBootInfo(BootInfoType type)
{
    return Arch_GetBootInfoFrom(type, nullptr);
}
BootInfoTag* Arch_GetBootInfoFrom(BootInfoType type, BootInfoTag* tag)
{
    if (!tag)
        tag = (BootInfoTag*)Arch_BootInfo.response->base;
    else
        tag = (BootInfoTag*)((uintptr_t)tag + tag->size);
    BootInfoTag* found = tag;
    while(found)
    {
        if (found->type == type)
            return found;
        found = (BootInfoTag*)((uintptr_t)found + found->size);
        if (found->type == BootInfoType_Last)
            found = nullptr;
    }
    return nullptr;
}
