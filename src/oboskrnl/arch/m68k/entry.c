/*
 * oboskrnl/arch/m68k/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
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

#include <mm/init.h>
#include <mm/swap.h>
#include <mm/bare_map.h>
#include <mm/context.h>

allocator_info* OBOS_KernelAllocator;
process *OBOS_KernelProcess;
timer_frequency CoreS_TimerFrequency;

volatile struct limine_memmap_request Arch_MemmapRequest = {
	.id = LIMINE_MEMMAP_REQUEST,
	.revision = 0
};
volatile struct limine_kernel_address_request Arch_KernelAddressRequest = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};
volatile struct limine_hhdm_request Arch_HHDMRequest = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};
volatile struct limine_kernel_file_request Arch_KernelFile = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
};
volatile struct limine_boot_info_request Arch_BootInfo = {
    .id = LIMINE_BOOT_INFO_REQUEST,
    .revision = 0,
};
cpu_local bsp_cpu;
thread kmain_thread;
thread idle_thread;
static thread_node kmain_node;
static thread_node idle_thread_node;
void Arch_InitializeVectorTable();
void Arch_RawRegisterInterrupt(uint8_t vec, uintptr_t f);

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
    OBOS_TextRendererState.fb.base = Arch_Framebuffer;
    OBOS_TextRendererState.fb.bpp = 32;
    OBOS_TextRendererState.fb.height = 768;
    OBOS_TextRendererState.fb.width = 1024;
    OBOS_TextRendererState.fb.pitch = 1024*4;
    OBOS_TextRendererState.fb.format = OBOS_FB_FORMAT_RGBX8888;
    memzero(Arch_Framebuffer, 1024*768*4);
    OBOS_TextRendererState.font = font_bin;
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
obos_status Arch_InitializeInitialSwapDevice(swap_dev* dev, void* buf, size_t size);
obos_status Arch_MapPage(uint32_t pt_root, uintptr_t virt, uintptr_t phys, uintptr_t ptFlags);
void Arch_PageFaultHandler(interrupt_frame* frame);
static basic_allocator kalloc;
struct stack_frame
{
    struct stack_frame* down;
    void* rip;
} fdssdfdsf;
void Arch_KernelEntry()
{
    Arch_RawRegisterInterrupt(0x2, (uintptr_t)Arch_PageFaultHandler);
    Arch_RawRegisterInterrupt(24, (uintptr_t)Arch_PICHandleSpurious);
    for (uint8_t vec = 25; vec < 32; vec++)
        Arch_RawRegisterInterrupt(vec, (uintptr_t)Arch_PICHandleIRQ);
    OBOS_Debug("%s: Initializing PMM.\n", __func__);
    Arch_InitializePMM();
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
    size_t swap_size = 16777216 /* 16 MiB */;
    uintptr_t swap_buf = Arch_AllocatePhysicalPages(swap_size/OBOS_PAGE_SIZE, 1, nullptr);
    Arch_InitializeInitialSwapDevice(&swap, Arch_MapToHHDM(swap_buf), swap_size);
	Mm_SwapProvider = &swap;
	Mm_Initialize();
    OBOS_Debug("%s: Initializing timer interface.\n", __func__);
    Core_InitializeTimerInterface();
    // Hang.
    while(1);
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