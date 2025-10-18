/*
 * oboskrnl/arch/x86_64/entry.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <kinit.h>
#include <error.h>
#include <klog.h>
#include <cmdline.h>
#include <font.h>
#include <text.h>

#define FLANTERM_IN_FLANTERM
#include <flanterm.h>
#include <flanterm_backends/fb.h>

#include <asan.h>

#include <vfs/tty.h>
#include <vfs/dirent.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <UltraProtocol/ultra_protocol.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/boot_info.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/timer.h>
#include <arch/x86_64/idt.h>
#include <arch/x86_64/cmos.h>
#include <arch/x86_64/interrupt_frame.h>

#include "gdbstub/connection.h"
#include "gdbstub/gdb_udp_backend.h"
#include "gdbstub/debug.h"

#include <uacpi_libc.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x40000];
extern char Arch_InitialISTStack[0x20000];
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;

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
    "\x1b[38;5;12m",
    "\x1b[38;5;10m",
    "\x1b[38;5;14m",
    "\x1b[38;5;9m",
    "\x1b[38;5;13m",
    "\x1b[38;5;11m",
    "\x1b[38;5;15m",
};

OBOS_NO_UBSAN stack_frame OBOSS_StackFrameNext(stack_frame curr)
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
OBOS_NO_UBSAN uintptr_t OBOSS_StackFrameGetPC(stack_frame curr)
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

static void e9_set_color(color c, void* unused)
{
    e9_out(color_to_ansi[c], strlen(color_to_ansi[c]), unused);
}
static void e9_reset_color(void* unused)
{
    e9_out("\x1b[0m", 4, unused);
}

const uintptr_t Arch_cpu_local_curr_offset = offsetof(cpu_local, curr);
const uintptr_t Arch_cpu_local_currentIrql_offset = offsetof(cpu_local, currentIrql);

struct flanterm_context* OBOS_FlantermContext = nullptr;
static void flanterm_cb_set_color(color c, void*)
{
    flanterm_write(OBOS_FlantermContext, color_to_ansi[c], strlen(color_to_ansi[c]));
}
static void flanterm_cb_reset_color(void*)
{
    flanterm_write(OBOS_FlantermContext, "\x1b[0m", 4);
}
static void flanterm_cb_out(const char* str, size_t sz, void*)
{
    flanterm_write(OBOS_FlantermContext, str, sz);
}
static void init_flanterm_backend()
{
    log_backend backend = {};
    backend.reset_color = flanterm_cb_reset_color;
    backend.set_color = flanterm_cb_set_color;
    backend.write = flanterm_cb_out;
    OBOS_AddLogSource(&backend);
}

static struct ultra_module_info_attribute* Arch_FlantermBumpAllocator;
static void *flanterm_malloc(size_t sz)
{
    if (!Arch_FlantermBumpAllocator)
        return nullptr;
    static size_t bump_off = 0;
    if ((bump_off + sz) > Arch_FlantermBumpAllocator->size)
        return nullptr;
    void* ret = (void*)(Arch_FlantermBumpAllocator->address + bump_off);
    bump_off += sz;
    return ret;
}
static void flanterm_free(void* blk, size_t sz)
{
    OBOS_UNUSED(blk && sz);
    return;
}

OBOS_PAGEABLE_FUNCTION void __attribute__((no_stack_protector)) Arch_KernelEntry(struct ultra_boot_context* bcontext)
{
    bsp_cpu.id = 0;
    bsp_cpu.isBSP = true;
    Core_CpuCount = 1;
    Core_CpuInfo = &bsp_cpu;
    Core_CpuInfo->curr = Core_CpuInfo;
    Core_CpuInfo->currentIrql = 0xf;

    extern uint64_t __stack_chk_guard;
    Core_CpuInfo->arch_specific.stack_check_guard = __stack_chk_guard;

    wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);

    ParseBootContext(bcontext);
    Arch_BootContext = bcontext;
    OBOS_ParseCMDLine();
    asm("sti");

    uint64_t log_level = OBOS_GetOPTD_Ex("log-level", LOG_LEVEL_LOG);
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
        // OBOS_TextRendererState.fg_color = 0xffffffff;
        // OBOS_TextRendererState.fb.base = Arch_MapToHHDM(Arch_Framebuffer->physical_address);
        // OBOS_TextRendererState.fb.bpp = Arch_Framebuffer->bpp;
        // OBOS_TextRendererState.fb.format = Arch_Framebuffer->format;
        // OBOS_TextRendererState.fb.height = Arch_Framebuffer->height;
        // OBOS_TextRendererState.fb.width = Arch_Framebuffer->width;
        // OBOS_TextRendererState.fb.pitch = Arch_Framebuffer->pitch;
        // for (size_t y = 0; y < Arch_Framebuffer->height; y++)
        //     for (size_t x = 0; x < Arch_Framebuffer->width; x++)
        //         OBOS_PlotPixel(OBOS_TEXT_BACKGROUND, &((uint8_t*)OBOS_TextRendererState.fb.base)[y*Arch_Framebuffer->pitch+x*Arch_Framebuffer->bpp/8], OBOS_TextRendererState.fb.format);
        // OBOS_TextRendererState.column = 0;
        // OBOS_TextRendererState.row = 0;
        // OBOS_TextRendererState.font = font_bin;
        // OBOS_AddLogSource(&OBOS_ConsoleOutputCallback);
        // if (Arch_Framebuffer->format == ULTRA_FB_FORMAT_INVALID)
        //     return;
        uint8_t red_mask_size = 8, red_mask_shift = 0;
        uint8_t green_mask_size = 8, green_mask_shift = 0;
        uint8_t blue_mask_size = 8, blue_mask_shift = 0;
        uint32_t bg = OBOS_TEXT_BACKGROUND;
        switch (Arch_Framebuffer->format) {
            case ULTRA_FB_FORMAT_BGR888:
                red_mask_shift = 16;
                green_mask_shift = 8;
                blue_mask_shift = 0;
                bg = __builtin_bswap32(bg) >> 8;
                break;
            case ULTRA_FB_FORMAT_RGB888:
                red_mask_shift = 0;
                green_mask_shift = 8;
                blue_mask_shift = 16;
                break;
            case ULTRA_FB_FORMAT_RGBX8888:
                red_mask_shift = 8;
                green_mask_shift = 16;
                blue_mask_shift = 24;
                break;
            case ULTRA_FB_FORMAT_XRGB8888:
                red_mask_shift = 0;
                green_mask_shift = 8;
                blue_mask_shift = 16;
                bg = bg >> 8;
                break;

        }
        static uint32_t ansi_colors[8] = {
            __builtin_bswap32(0x000000) >> 8,
            __builtin_bswap32(0xbb0000) >> 8,
            __builtin_bswap32(0x00bb00) >> 8,
            __builtin_bswap32(0xbbbb00) >> 8,
            __builtin_bswap32(0x0000bb) >> 8,
            __builtin_bswap32(0xbb00bb) >> 8,
            __builtin_bswap32(0x00bbbb) >> 8,
            __builtin_bswap32(0xbbbbbb) >> 8,
        };
        static uint32_t ansi_bright_colors[8] = {
            __builtin_bswap32(0x555555) >> 8,
            __builtin_bswap32(0xff5555) >> 8,
            __builtin_bswap32(0x55ff55) >> 8,
            __builtin_bswap32(0xffff55) >> 8,
            __builtin_bswap32(0x5555aa) >> 8,
            __builtin_bswap32(0xff55ff) >> 8,
            __builtin_bswap32(0x55ffff) >> 8,
            __builtin_bswap32(0xffffff) >> 8,

        };
        OBOS_FlantermContext = flanterm_fb_init(
            flanterm_malloc, flanterm_free, 
            Arch_MapToHHDM(Arch_Framebuffer->physical_address), Arch_Framebuffer->width, Arch_Framebuffer->height, Arch_Framebuffer->pitch, 
            red_mask_size, red_mask_shift, 
            green_mask_size, green_mask_shift, 
            blue_mask_size, blue_mask_shift, 
            nullptr, ansi_colors, 
            ansi_bright_colors, nullptr, 
            nullptr, nullptr,
            nullptr, 
            (void*)font_bin, 8, 16, 0, 
            0,0,
            0);
        // while(1);
        init_flanterm_backend();
    }

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
            else if (strcmp(module->name, "FLANTERM_BUFF"))
                Arch_FlantermBumpAllocator = module;
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
    if (!Arch_FlantermBumpAllocator)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find the FLANTERM_BUFF module in boot context!\nMake sure to pass a module named FLANTERM_BUFF with a size big enough for width*pitch?\n");
}

extern obos_status Arch_InitializeKernelPageTable();

// uint64_t random_number();
// uint8_t random_number8();
// __asm__(
//     "random_number:; rdrand %rax; ret; "
//     "random_number8:; rdrand %ax; mov $0, %ah; ret; "
// );

void Arch_SMPStartup();

void OBOSS_KernelPostIRQInit()
{
    obos_status status = OBOS_STATUS_SUCCESS;

    OBOS_Debug("%s: Initializing CMOS RTC\n", __func__);
    Arch_CMOSInitialize();
    OBOS_Debug("%s: Initializing scheduler timer.\n", __func__);
    Arch_InitializeSchedulerTimer();
    OBOS_Debug("%s: Initializing IOAPICs.\n", __func__);
    if (obos_is_error(status = Arch_InitializeIOAPICs()))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not initialize I/O APICs. Status: %d\n", status);
}

void OBOSS_KernelPostVMMInit()
{
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
        ((struct flanterm_fb_context*)OBOS_FlantermContext)->framebuffer = base_;
        OBOS_TextRendererState.fb.base = base_;
        OBOS_TextRendererState.fb.height = Arch_Framebuffer->height;
        OBOS_TextRendererState.fb.pitch = Arch_Framebuffer->pitch;
        OBOS_TextRendererState.fb.width = Arch_Framebuffer->width;
        OBOS_TextRendererState.fb.bpp = Arch_Framebuffer->bpp;
        OBOS_TextRendererState.fb.format = Arch_Framebuffer->format;
    }
}

void OBOSS_KernelPostPMMInit()
{
    OBOS_Debug("%s: Initializing page tables.\n", __func__);
    obos_status status = Arch_InitializeKernelPageTable();
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize page tables. Status: %d.\n", status);
    bsp_idleThread.context.cr3 = getCR3();
}

void OBOSS_KernelPostKProcInit()
{
    Core_ProcessAppendThread(OBOS_KernelProcess, &kernelMainThread);
    Core_ProcessAppendThread(OBOS_KernelProcess, &bsp_idleThread);
}

void OBOSS_InitializeSMP()
{
    OBOS_Debug("%s: Initializing LAPIC.\n", __func__);
    Arch_LAPICInitialize(true);
    OBOS_Debug("%s: Initializing SMP.\n", __func__);
    Arch_SMPStartup();
    bsp_idleThread.context.gs_base = rdmsr(0xC0000101 /* GS_BASE */);
    bsp_idleThread.masterCPU = CoreS_GetCPULocalPtr();
    Core_GetCurrentThread()->masterCPU = CoreS_GetCPULocalPtr();
}

static void ultra_module_to_boot_module(const struct ultra_module_info_attribute *ultra_module, struct boot_module* out_module)
{
    if (!ultra_module || !out_module)
        return;
    out_module->address = (void*)ultra_module->address;
    out_module->size = ultra_module->size;
    out_module->name = ultra_module->name;
    out_module->is_memory = ultra_module->type == ULTRA_MODULE_TYPE_MEMORY;
    out_module->is_kernel = strcmp(ultra_module->name, "__KERNEL__");
    return;
}

void OBOSS_GetModule(struct boot_module *module, const char* name)
{
    ultra_module_to_boot_module(FindBootModule(Arch_BootContext, name, strlen(name)), module);
}
void OBOSS_GetModuleLen(struct boot_module *module, const char* name, size_t name_len)
{
    ultra_module_to_boot_module(FindBootModule(Arch_BootContext, name, name_len), module);
}
void OBOSS_GetKernelModule(struct boot_module *module)
{
    ultra_module_to_boot_module((void*)Arch_KernelBinary, module);
}

void OBOSS_MakeTTY()
{
    dirent* ps2k1 = VfsH_DirentLookup("/dev/ps2k1");
    if (ps2k1)
    {
        tty_interface i = {};
        VfsH_MakeScreenTTY(&i, ps2k1->vnode, nullptr, OBOS_FlantermContext);
        dirent* tty = nullptr;
        Vfs_RegisterTTY(&i, &tty, false);
        Core_GetCurrentThread()->proc->controlling_tty = tty->vnode->data;
    }
}

static bool isnum(char ch)
{
    return (ch - '0') >= 0 && (ch - '0') < 10;
}

#include <net/tables.h>

void Arch_KernelMainBootstrap()
{
    OBOS_KernelInit();
    Core_ExitCurrentThread();
}
