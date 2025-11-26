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
#include <stddef.h>
#include <memmanip.h>
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

#include <uacpi_libc.h>

#include <mm/bare_map.h>

#include <arch/x86_64/pmm.h>

#if OBOS_USE_LIMINE
#   include <limine.h>
#endif

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x40000];
extern char Arch_InitialISTStack[0x20000];
static thread bsp_idleThread;

static thread kernelMainThread;
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
static bool disable_flanterm_backend = false;
static void flanterm_cb_set_color(color c, void*)
{
    if (disable_flanterm_backend) return;
    flanterm_write(OBOS_FlantermContext, color_to_ansi[c], strlen(color_to_ansi[c]));
}
static void flanterm_cb_reset_color(void*)
{
    if (disable_flanterm_backend) return;
    flanterm_write(OBOS_FlantermContext, "\x1b[0m", 4);
}
static void flanterm_cb_out(const char* str, size_t sz, void*)
{
    if (disable_flanterm_backend) return;
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

static struct {
    uintptr_t address;
    uintptr_t size;
} Arch_FlantermBumpAllocator = {};
static void *flanterm_malloc(size_t sz)
{
    static size_t bump_off = 0;
    if ((bump_off + sz) > Arch_FlantermBumpAllocator.size)
        return nullptr;
    void* ret = (void*)(Arch_FlantermBumpAllocator.address + bump_off);
    bump_off += sz;
    return ret;
}
static void flanterm_free(void* blk, size_t sz)
{
    OBOS_UNUSED(blk && sz);
    return;
}

#if OBOS_USE_LIMINE
volatile struct limine_framebuffer_request Arch_LimineFBRequest = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 1,
};
volatile struct limine_memmap_request Arch_LimineMemmapRequest = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_module_request Arch_LimineModuleRequest = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 1,
};
volatile struct limine_hhdm_request Arch_LimineHHDMRequest = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_executable_file_request Arch_LimineKernelInfoRequest = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_executable_address_request Arch_LimineKernelAddressRequest = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_executable_cmdline_request Arch_LimineKernelCmdlineRequest = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_rsdp_request Arch_LimineRSDPRequest = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};
volatile struct limine_bootloader_info_request Arch_LimineBtldrInfoRequest = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
};
volatile struct ultra_framebuffer limine_fb0 = {};
volatile struct ultra_framebuffer* Arch_Framebuffer = &limine_fb0;
struct limine_file* Arch_KernelBinary = nullptr;
#endif

uintptr_t Arch_RSDPBase = 0;

#if !OBOS_USE_LIMINE
OBOS_PAGEABLE_FUNCTION void __attribute__((no_stack_protector)) Arch_KernelEntry(struct ultra_boot_context* bcontext)
#else
OBOS_PAGEABLE_FUNCTION void __attribute__((no_stack_protector)) Arch_KernelEntry()
#endif
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

#if !OBOS_USE_LIMINE
    ParseBootContext(bcontext);
    Arch_BootContext = bcontext;
#else
    OBOS_KernelCmdLine = Arch_LimineKernelCmdlineRequest.response->cmdline;
    Arch_KernelBinary = Arch_LimineKernelInfoRequest.response->executable_file;
    Arch_RSDPBase = Arch_UnmapFromHHDM(Arch_LimineRSDPRequest.response->address);
#endif

    basicmm_region bump_region = {};
    struct boot_module bump_region_module = {};
    OBOSS_GetModule(&bump_region_module, "BUMP_REGION");
    if (!bump_region_module.address)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find BUMP_REGION kernel module\n");
    bump_region.addr = (uintptr_t)bump_region_module.address;
    bump_region.size = bump_region_module.size;
    OBOSH_BasicMMSetBumpRegion(&bump_region);

    struct boot_module flanterm_buff = {};
    OBOSS_GetModule(&flanterm_buff, "FLANTERM_BUFF");
    if (!flanterm_buff.address)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find FLANTERM_BUFF kernel module\n");
    Arch_FlantermBumpAllocator.address = (uintptr_t)flanterm_buff.address;
    Arch_FlantermBumpAllocator.size = flanterm_buff.size;
    
    OBOS_ParseCMDLine();
    asm("sti");

    uint64_t log_level = OBOS_GetOPTD_Ex("log-level", LOG_LEVEL_LOG);
    if (log_level > 4)
        log_level = LOG_LEVEL_DEBUG;
    OBOS_SetLogLevel(log_level);
    extern uint64_t Arch_KernelCR3;
    Arch_KernelCR3 = getCR3();

#if 0
    init_serial_log_backend();
#endif

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
    OBOS_UNUSED(ansi_bright_colors && ansi_colors);
#if !OBOS_USE_LIMINE
    if (!Arch_Framebuffer)
        OBOS_Warning("No framebuffer passed by the bootloader. All kernel logs will be on port 0xE9.\n");
    else
    {
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
#else
    if (Arch_LimineFBRequest.response->framebuffer_count)
    {
        struct limine_framebuffer *fb = Arch_LimineFBRequest.response->framebuffers[0];
        OBOS_FlantermContext = flanterm_fb_init(
            flanterm_malloc, flanterm_free, 
            fb->address, fb->width, fb->height, fb->pitch, 
            fb->red_mask_size, fb->red_mask_shift, 
            fb->green_mask_size, fb->green_mask_shift, 
            fb->blue_mask_size, fb->blue_mask_shift, 
            nullptr, nullptr, 
            nullptr, nullptr, 
            nullptr, nullptr,
            nullptr, 
            (void*)font_bin, 8, 16, 0, 
            0,0,
            0);
        init_flanterm_backend();
        limine_fb0.physical_address = Arch_UnmapFromHHDM(fb->address);
        limine_fb0.bpp = fb->bpp;
        limine_fb0.width = fb->width;
        limine_fb0.height = fb->height;
        limine_fb0.pitch = fb->pitch;
        do {
            if (limine_fb0.bpp == 24)
            {
                if (fb->blue_mask_shift == 0 && fb->green_mask_shift == 8 && fb->red_mask_shift == 16)
                    limine_fb0.format = ULTRA_FB_FORMAT_RGB888;
                if (fb->red_mask_shift == 0 && fb->green_mask_shift == 8 && fb->blue_mask_shift == 16)
                    limine_fb0.format = ULTRA_FB_FORMAT_BGR888;
                break;
            } 
            else if (limine_fb0.bpp == 32) 
            {
                uint8_t x_shift = 0;
                if (fb->red_mask_shift == 16) x_shift = 24;
                else if (fb->red_mask_shift == 24) x_shift = 0;
                if (x_shift == 0 && fb->blue_mask_shift == 8 && fb->green_mask_shift == 16 && fb->red_mask_shift == 24)
                    limine_fb0.format = ULTRA_FB_FORMAT_RGBX8888;
                if (fb->blue_mask_shift == 0 && fb->green_mask_shift == 8 && fb->red_mask_shift == 16 && x_shift == 24)
                    limine_fb0.format = ULTRA_FB_FORMAT_XRGB8888;
                break;
            }
        } while(0);
    }
#endif


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

#if OBOS_USE_LIMINE
    OBOS_Log("Bootloader is %s-%s\n", Arch_LimineBtldrInfoRequest.response->name, Arch_LimineBtldrInfoRequest.response->version);
#else
    OBOS_Log("Bootloader is %.32s\n", Arch_LdrPlatformInfo->loader_name, Arch_LdrPlatformInfo->loader_major, Arch_LdrPlatformInfo->loader_minor);
#endif

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
    CoreH_ThreadReady(&kernelMainThread);
    CoreH_ThreadReady(&bsp_idleThread);
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

#if !OBOS_USE_LIMINE
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
    Arch_RSDPBase = Arch_LdrPlatformInfo->acpi_rsdp_address;
}
#endif

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

#if !OBOS_USE_LIMINE
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
#else
static void limine_file_to_boot_module(const struct limine_file *file, struct boot_module* out_module)
{
    if (!file || !out_module)
        return;
    out_module->address = file->address;
    out_module->size = file->size;
    out_module->name = file->string;
    out_module->is_memory = false;
    out_module->is_kernel = file == Arch_LimineKernelInfoRequest.response->executable_file;
    return;
}
static struct limine_file* find_module(const char* name, size_t name_len)
{
    for (size_t i = 0; i < Arch_LimineModuleRequest.response->module_count; i++)
    {
        struct limine_file* cur = Arch_LimineModuleRequest.response->modules[i];
        if (strncmp(cur->string, name, name_len))
            return cur;
    }
    return nullptr;
}

void OBOSS_GetModule(struct boot_module *module, const char* name)
{
    limine_file_to_boot_module(find_module(name, strlen(name)), module);
}
void OBOSS_GetModuleLen(struct boot_module *module, const char* name, size_t name_len)
{
    limine_file_to_boot_module(find_module(name, name_len), module);
}
void OBOSS_GetKernelModule(struct boot_module *module)
{
    limine_file_to_boot_module(Arch_LimineKernelInfoRequest.response->executable_file, module);
}
#endif

void OBOSS_MakeTTY()
{
    Core_SetProcessGroup(Core_GetCurrentThread()->proc, 0);
    dirent* ps2k1 = VfsH_DirentLookup("/dev/ps2k1");
    if (ps2k1)
    {
        tty_interface i = {};
        VfsH_MakeScreenTTY(&i, ps2k1->vnode, nullptr, OBOS_FlantermContext);
        dirent* tty = nullptr;
        Vfs_RegisterTTY(&i, &tty, false);
        process_group* pgrp = Core_GetCurrentThread()->proc->pgrp;
        ((struct tty*)tty->vnode->desc)->fg_job = pgrp;
        pgrp->controlling_tty = (void*)tty->vnode->desc;
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
    // No longer receive logs on the TTY, if the option is set
    // Do __not__ document this option.
    if (OBOS_GetOPTF("disable-flanterm-logging"))
        disable_flanterm_backend = true;
    Core_ExitCurrentThread();
}
