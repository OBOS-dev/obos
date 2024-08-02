/*
 * oboskrnl/arch/x86_64/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <stdint.h>
#include <struct_packing.h>
#include <memmanip.h>
#include <text.h>
#include <font.h>

#include <UltraProtocol/ultra_protocol.h>

#include <arch/x86_64/idt.h>
#include <arch/x86_64/interrupt_frame.h>
#include <arch/x86_64/hpet_table.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <irq/timer.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/pmm.h>

#include <driver_interface/driverId.h>
#include <driver_interface/loader.h>
#include <driver_interface/header.h>
#include <driver_interface/pnp.h>

#include <allocators/basic_allocator.h>
#include <allocators/base.h>

#include <arch/x86_64/boot_info.h>

#include <arch/x86_64/lapic.h>
#include <arch/x86_64/ioapic.h>

#include <irq/irq.h>

#include <mm/bare_map.h>
#include <mm/init.h>
#include <mm/swap.h>
#include <mm/context.h>
#include <mm/handler.h>
#include <mm/alloc.h>

#include <scheduler/process.h>

#include <stdatomic.h>

#include <utils/tree.h>

#include <external/fixedptc.h>

#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/sleep.h>
#include <uacpi/event.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x40000];
extern char Arch_InitialISTStack[0x20000];
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;
static swap_dev swap;

static thread kernelMainThread;
static thread_node kernelMainThreadNode;

static cpu_local bsp_cpu;
extern void Arch_IdleTask();
void Arch_CPUInitializeGDT(cpu_local* info, uintptr_t istStack, size_t istStackSize);
void Arch_KernelMainBootstrap();
static void ParseBootContext(struct ultra_boot_context* bcontext);
void Arch_PageFaultHandler(interrupt_frame* frame);
void Arch_DoubleFaultHandler(interrupt_frame* frame);
struct stack_frame
{
	struct stack_frame* down;
	void* rip;
} volatile blahblahblah____;
OBOS_PAGEABLE_FUNCTION void Arch_KernelEntry(struct ultra_boot_context* bcontext)
{
	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	Core_GetIrql();
	ParseBootContext(bcontext);
	asm("sti");
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
		OBOS_TextRendererState.column = 0;
		OBOS_TextRendererState.row = 0;
		OBOS_TextRendererState.font = font_bin;
		if (Arch_Framebuffer->format == ULTRA_FB_FORMAT_INVALID)
			return;
	}
	if (Arch_LdrPlatformInfo->page_table_depth != 4)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "5-level paging is unsupported by oboskrnl.\n");
#if OBOS_RELEASE
	// OBOS_SetLogLevel(LOG_LEVEL_LOG);
	OBOS_Log("Booting OBOS %s committed on %s. Build time: %s.\n", GIT_SHA1, GIT_DATE, __DATE__ " " __TIME__);
	char cpu_vendor[13] = {0};
	memset(cpu_vendor, 0, 13);
	__cpuid__(0, 0, nullptr, (uint32_t*)&cpu_vendor[0],(uint32_t*)&cpu_vendor[8], (uint32_t*)&cpu_vendor[4]);
	uint32_t ecx = 0;
	__cpuid__(1, 0, nullptr, nullptr, &ecx, nullptr);
	bool isHypervisor = ecx & (1<<31) /* Hypervisor bit: Always 0 on physical CPUs. */;
	char brand_string[49];
	memset(brand_string, 0, sizeof(brand_string));
	__cpuid__(0x80000002, 0, (uint32_t*)&brand_string[0], (uint32_t*)&brand_string[4], (uint32_t*)&brand_string[8], (uint32_t*)&brand_string[12]);
	__cpuid__(0x80000003, 0, (uint32_t*)&brand_string[16], (uint32_t*)&brand_string[20], (uint32_t*)&brand_string[24], (uint32_t*)&brand_string[28]);
	__cpuid__(0x80000004, 0, (uint32_t*)&brand_string[32], (uint32_t*)&brand_string[36], (uint32_t*)&brand_string[40], (uint32_t*)&brand_string[44]);
	OBOS_Log("Running on a %s processor, cpu brand string, %s. We are currently %srunning on a hypervisor\n", cpu_vendor, brand_string, isHypervisor ? "" : "not ");
#endif
	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();
	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_RawRegisterInterrupt(0xe, (uintptr_t)Arch_PageFaultHandler);
	Arch_RawRegisterInterrupt(0x8, (uintptr_t)Arch_DoubleFaultHandler);
	Arch_InitializeIDT(true);
	OBOS_Debug("Enabling XD bit in IA32_EFER.\n");
	{
		uint32_t edx = 0;
		__cpuid__(0x80000001, 0, nullptr, nullptr, nullptr, &edx);
		if (edx & (1 << 20))
			wrmsr(0xC0000080 /* IA32_EFER */, rdmsr(0xC0000080) | (1<<11) /* XD Enable */);
	}
	OBOS_Debug("%s: Initializing scheduler.\n", __func__);
	bsp_cpu.id = 0;
	bsp_cpu.isBSP = true;
	Core_CpuCount = 1;
	Core_CpuInfo = &bsp_cpu;
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
	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);
	Core_CpuInfo[0].currentIrql = Core_GetIrql();
	Core_CpuInfo[0].arch_specific.ist_stack = Arch_InitialISTStack;
	for (thread_priority i = 0; i <= THREAD_PRIORITY_MAX_VALUE; i++)
		Core_CpuInfo[0].priorityLists[i].priority = i;
	Core_CpuInfo->initialized = true;
	// Finally yield into the scheduler.
	OBOS_Debug("Yielding into the scheduler!\n");
	Core_Yield();
	OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Scheduler did not switch to a new thread.\n");
	while (1)
		asm volatile("nop" : : :);
}
struct ultra_memory_map_attribute* Arch_MemoryMap;
struct ultra_platform_info_attribute* Arch_LdrPlatformInfo;
struct ultra_kernel_info_attribute* Arch_KernelInfo;
struct ultra_module_info_attribute* Arch_KernelBinary;
struct ultra_module_info_attribute* Arch_InitialSwapBuffer;
struct ultra_module_info_attribute* Arch_UARTDriver;
struct ultra_framebuffer* Arch_Framebuffer;
const char* OBOS_KernelCmdLine;
extern obos_status Arch_InitializeInitialSwapDevice(swap_dev* dev, void* buf, size_t size);
static OBOS_PAGEABLE_FUNCTION OBOS_NO_KASAN void ParseBootContext(struct ultra_boot_context* bcontext)
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
			else if (strcmp(module->name, "INITIAL_SWAP_BUFFER"))
				Arch_InitialSwapBuffer = module;
			else if (strcmp(module->name, "uart_driver"))
				Arch_UARTDriver = module;
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
	if (!Arch_InitialSwapBuffer)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find the initial swap module in the boot context!\n\n");
}
extern obos_status Arch_InitializeKernelPageTable();
uintptr_t Arch_GetPML2Entry(uintptr_t pml4Base, uintptr_t addr);
OBOS_NO_UBSAN void Arch_PageFaultHandler(interrupt_frame* frame)
{
	uintptr_t virt = getCR2();
	virt &= ~0xfff;
	if (Arch_GetPML2Entry(getCR3(), virt) & (1<<7))
		virt &= ~0x1fffff;
	if (Mm_IsInitialized())
	{
		CoreS_GetCPULocalPtr()->arch_specific.pf_handler_running = true;
		uint32_t mm_ec = 0;
		if (frame->errorCode & BIT(0))
			mm_ec |= PF_EC_PRESENT;
		if (frame->errorCode & BIT(1))
			mm_ec |= PF_EC_RW;
		if (frame->errorCode & BIT(2))
			mm_ec |= PF_EC_UM;
		if (frame->errorCode & BIT(3))
			mm_ec |= PF_EC_INV_PTE;
		if (frame->errorCode & BIT(4))
			mm_ec |= PF_EC_EXEC;
		obos_status status = Mm_HandlePageFault(CoreS_GetCPULocalPtr()->currentContext, virt, mm_ec);
		switch (status)
		{
			case OBOS_STATUS_SUCCESS:
				CoreS_GetCPULocalPtr()->arch_specific.pf_handler_running = false;
				OBOS_ASSERT(frame->rsp != 0);
				return;
			case OBOS_STATUS_UNHANDLED:
				break;
			default:
			{
				static const char format[] = "Handling page fault with error code 0x%x on address %p failed.\n";
				OBOS_Warning(format, mm_ec, getCR2());
				break;
			}
		}
	}
	page* volatile pg = nullptr;
	if (CoreS_GetCPULocalPtr()->currentContext)
	{
		page what;
		what.addr = virt;
		pg = RB_FIND(page_tree, &CoreS_GetCPULocalPtr()->currentContext->pages, &what);
	}
	OBOS_Panic(OBOS_PANIC_EXCEPTION, 
		"Page fault at 0x%p in %s-mode while to %s page at 0x%p, which is %s. Error code: %d\n"
		"Register dump:\n"
		"\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
		"\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
		"\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
		"\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
		"\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
		"\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
		"\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
		"\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
		"\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n",
		(void*)frame->rip,
		frame->cs == 0x8 ? "kernel" : "user",
		(frame->errorCode & 2) ? "write" : ((frame->errorCode & 0x10) ? "execute" : "read"),
		getCR2(),
		frame->errorCode & 1 ? "present" : "unpresent",
		frame->errorCode,
		frame->rdi, frame->rsi, frame->rbp,
		frame->rsp, frame->rbx, frame->rdx,
		frame->rcx, frame->rax, frame->rip,
		frame->r8, frame->r9, frame->r10,
		frame->r11, frame->r12, frame->r13,
		frame->r14, frame->r15, frame->rflags,
		frame->ss, frame->ds, frame->cs,
		getCR0(), getCR2(), frame->cr3,
		getCR4(), Core_GetIrql(), getEFER()
	);
}
OBOS_NO_UBSAN OBOS_NO_KASAN void Arch_DoubleFaultHandler(interrupt_frame* frame)
{
	static const char format[] = "Double fault!\n"
		"Register dump:\n"
		"\tRDI: 0x%016lx, RSI: 0x%016lx, RBP: 0x%016lx\n"
		"\tRSP: 0x%016lx, RBX: 0x%016lx, RDX: 0x%016lx\n"
		"\tRCX: 0x%016lx, RAX: 0x%016lx, RIP: 0x%016lx\n"
		"\t R8: 0x%016lx,  R9: 0x%016lx, R10: 0x%016lx\n"
		"\tR11: 0x%016lx, R12: 0x%016lx, R13: 0x%016lx\n"
		"\tR14: 0x%016lx, R15: 0x%016lx, RFL: 0x%016lx\n"
		"\t SS: 0x%016lx,  DS: 0x%016lx,  CS: 0x%016lx\n"
		"\tCR0: 0x%016lx, CR2: 0x%016lx, CR3: 0x%016lx\n"
		"\tCR4: 0x%016lx, CR8: 0x%016x, EFER: 0x%016lx\n";
	OBOS_Panic(OBOS_PANIC_EXCEPTION, 
		format,
		frame->rdi, frame->rsi, frame->rbp,
		frame->rsp, frame->rbx, frame->rdx,
		frame->rcx, frame->rax, frame->rip,
		frame->r8, frame->r9, frame->r10,
		frame->r11, frame->r12, frame->r13,
		frame->r14, frame->r15, frame->rflags,
		frame->ss, frame->ds, frame->cs,
		getCR0(), getCR2(), frame->cr3,
		getCR4(), Core_GetIrql(), getEFER()
	);
}
uint64_t random_number();
uint8_t random_number8();
__asm__(
	"random_number:; rdrand %rax; ret; "
	"random_number8:; rdrand %ax; mov $0, %ah; ret; "
);
allocator_info* OBOS_KernelAllocator;
static basic_allocator kalloc;
void Arch_SMPStartup();
extern uint64_t Arch_FindCounter(uint64_t hz);
atomic_size_t nCPUsWithInitializedTimer;
void Arch_SchedulerIRQHandlerEntry(irq* obj, interrupt_frame* frame, void* userdata, irql oldIrql)
{
	OBOS_UNUSED(obj);
	OBOS_UNUSED(frame);
	OBOS_UNUSED(userdata);
	OBOS_UNUSED(oldIrql);
	if (!CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer)
	{
		Arch_LAPICAddress->lvtTimer = 0x20000 | (Core_SchedulerIRQ->vector->id + 0x20);
		Arch_LAPICAddress->divideConfig = 0b1101;
		Arch_LAPICAddress->initialCount = Arch_FindCounter(Core_SchedulerTimerFrequency);
		OBOS_Debug("Initialized timer for CPU %d.\n", CoreS_GetCPULocalPtr()->id);
		CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer = true;
		nCPUsWithInitializedTimer++;
		// TODO: Move the PAT initialization code somewhere else.
		wrmsr(0x277, 0x0001040600070406);
		// UC UC- WT WB UC WC WT WB

	}
	else
		Core_Yield();
}
HPET* Arch_HPETAddress;
uint64_t Arch_HPETFrequency;
timer_frequency CoreS_TimerFrequency;
uint64_t Arch_CalibrateHPET(uint64_t freq)
{
	if (!Arch_HPETFrequency)
		Arch_HPETFrequency = 1000000000000000 / Arch_HPETAddress->generalCapabilitiesAndID.counterCLKPeriod;
	Arch_HPETAddress->generalConfig &= ~(1 << 0);
	uint64_t compValue = Arch_HPETAddress->mainCounterValue + (Arch_HPETFrequency / freq);
	Arch_HPETAddress->timer0.timerConfigAndCapabilities &= ~(1 << 2);
	Arch_HPETAddress->timer0.timerConfigAndCapabilities &= ~(1 << 3);
	return compValue;
}
#define OffsetPtr(ptr, off, t) ((t*)(((uintptr_t)(ptr)) + (off)))
static OBOS_PAGEABLE_FUNCTION OBOS_NO_UBSAN void InitializeHPET()
{
	extern obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
	static basicmm_region hpet_region;
	hpet_region.mmioRange = true;
	ACPIRSDPHeader* rsdp = (ACPIRSDPHeader*)Arch_MapToHHDM(Arch_LdrPlatformInfo->acpi_rsdp_address);
	bool tables32 = rsdp->Revision == 0;
	ACPISDTHeader* xsdt = tables32 ? (ACPISDTHeader*)(uintptr_t)rsdp->RsdtAddress : (ACPISDTHeader*)rsdp->XsdtAddress;
	xsdt = (ACPISDTHeader*)Arch_MapToHHDM((uintptr_t)xsdt);
	size_t nEntries = (xsdt->Length - sizeof(*xsdt)) / (tables32 ? 4 : 8);
	HPET_Table* hpet_table = nullptr;
	for (size_t i = 0; i < nEntries; i++)
	{
		uintptr_t phys = tables32 ? OffsetPtr(xsdt, sizeof(*xsdt), uint32_t)[i] : OffsetPtr(xsdt, sizeof(*xsdt), uint64_t)[i];
		ACPISDTHeader* header = (ACPISDTHeader*)Arch_MapToHHDM(phys);
		if (memcmp(header->Signature, "HPET", 4))
		{
			hpet_table = (HPET_Table*)header;
			break;
		}
	}
	if (!hpet_table)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No HPET!\n");
	uintptr_t phys = hpet_table->baseAddress.address;
	Arch_HPETAddress = (HPET*)0xffffffffffffd000;
	Arch_MapPage(getCR3(), Arch_HPETAddress, phys, 0x8000000000000013);
	OBOSH_BasicMMAddRegion(&hpet_region, Arch_HPETAddress, 0x1000);
}
static void hpet_irq_move_callback(irq* i, irq_vector* from, irq_vector* to, void* userdata)
{
	OBOS_UNUSED(i);
	OBOS_UNUSED(from);
	HPET_Timer* timer = (HPET_Timer*)userdata;
	OBOS_ASSERT(timer);
	uint32_t gsi = (timer->timerConfigAndCapabilities >> 9) & 0b11111;
	Arch_IOAPICMapIRQToVector(gsi, to->id+0x20, false, TriggerModeLevelSensitive);
}
OBOS_NO_KASAN OBOS_NO_UBSAN void hpet_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
	// HPET_Timer* timer = (HPET_Timer*)i->irqCheckerUserdata;
	// OBOS_ASSERT(timer);
	// size_t timerIndex = ((uintptr_t)timer-(uintptr_t)Arch_HPETAddress-offsetof(HPET, timer0))/sizeof(HPET_Timer);
	// Arch_HPETAddress->generalInterruptStatus &= ~(1<<timerIndex);
	((irq_handler)userdata)(i, frame, nullptr, oldIrql);
}
OBOS_PAGEABLE_FUNCTION obos_status CoreS_InitializeTimer(irq_handler handler)
{
	static bool initialized = false;
	OBOS_ASSERT(!initialized);
	if (initialized)
		return OBOS_STATUS_ALREADY_INITIALIZED;
	if (!handler)
		return OBOS_STATUS_INVALID_ARGUMENT;
	obos_status status = Core_IrqObjectInitializeIRQL(Core_TimerIRQ, IRQL_TIMER, false, false);
	if (obos_is_error(status))
		return status;
	Core_TimerIRQ->moveCallback  = hpet_irq_move_callback;
	Core_TimerIRQ->handler = hpet_irq_handler;
	Core_TimerIRQ->handlerUserdata = handler;
	volatile HPET_Timer* timer = &Arch_HPETAddress->timer0;
	// TODO: Make this support choosing a different timer.
	if (!(timer->timerConfigAndCapabilities & (1<<4)))
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer does not support periodic mode.");
	if (!(timer->timerConfigAndCapabilities & (1<<5)))
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer is not a 64-bit timer.");
	Core_TimerIRQ->irqCheckerUserdata = (void*)timer;
	Core_TimerIRQ->irqMoveCallbackUserdata = (void*)timer;
	uint32_t irqRouting = timer->timerConfigAndCapabilities >> 32;
	if (!irqRouting)
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "HPET Timer does not support irq routing through the I/O APIC.");
	uint32_t gsi = UINT32_MAX;
	do {
		uint32_t cgsi = __builtin_ctz(irqRouting);
		if (Arch_IOAPICGSIUsed(cgsi) == OBOS_STATUS_SUCCESS)
		{
			gsi = cgsi;
			break;
		}
		irqRouting &= (1<cgsi);
	} while (irqRouting);
	if (gsi == UINT32_MAX)
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not find empty I/O APIC IRQ for the HPET.");
	OBOS_ASSERT(gsi <= 32);
	timer->timerConfigAndCapabilities |= (1<6)|(1<<3)|((uint8_t)gsi<<9); // Edge-triggered IRQs, set GSI, Periodic timer
	CoreS_TimerFrequency = 500;
	OBOS_Debug("HPET frequency: %ld, configured HPET frequency: %ld\n", Arch_HPETFrequency, CoreS_TimerFrequency);
	const uint64_t value = Arch_HPETFrequency/CoreS_TimerFrequency;
	timer->timerComparatorValue = Arch_HPETAddress->mainCounterValue + value;
	timer->timerComparatorValue = value;
	timer->timerConfigAndCapabilities |= (1<<1); // Enable IRQs
	Arch_IOAPICMapIRQToVector(gsi, Core_TimerIRQ->vector->id+0x20, true, TriggerModeEdgeSensitive);
	Arch_IOAPICMaskIRQ(gsi, false);
	Arch_HPETAddress->generalConfig = 0b01;
	initialized = true;
	Core_TimerDispatchThread->affinity &= ~1;
	return OBOS_STATUS_SUCCESS;
}
static uint64_t cached_divisor = 0;
timer_tick CoreS_GetTimerTick()
{
	if (!cached_divisor)
		cached_divisor = Arch_HPETFrequency/CoreS_TimerFrequency;
	return Arch_HPETAddress->mainCounterValue/cached_divisor;
}
uint64_t CoreS_TimerTickToNS(timer_tick tp)
{
	// 1/freq*1000000000*tp
    fixedptd ns = fixedpt_fromint(1); // 1.0
    const fixedptd divisor = fixedpt_fromint(CoreS_TimerFrequency); // freq
    ns = fixedpt_xdiv(ns, divisor);
    ns = fixedpt_xmul(ns, fixedpt_fromint(1000000000));
    ns = fixedpt_xmul(ns, fixedpt_fromint(tp));
	return fixedpt_toint(ns);	
}
process* OBOS_KernelProcess;
extern bool Arch_MakeIdleTaskSleep;
static uacpi_interrupt_ret handle_power_button(uacpi_handle ctx)
{
	OBOS_UNUSED(ctx);
	OBOS_Log("%s: Power button pressed. Requesting system shutdown...\n", __func__);
	uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
	asm("cli");
	uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
	return UACPI_INTERRUPT_HANDLED;
}
void Arch_KernelMainBootstrap()
{
	//Core_Yield();
	irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
	OBOS_Debug("%s: Initializing PMM.\n", __func__);
	Arch_InitializePMM();
	OBOS_Debug("%s: Initializing page tables.\n", __func__);
	obos_status status = Arch_InitializeKernelPageTable();
	if (obos_is_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize page tables. Status: %d.\n", status);
	bsp_idleThread.context.cr3 = getCR3();
	OBOS_Debug("%s: Initializing allocator...\n", __func__);
	OBOSH_ConstructBasicAllocator(&kalloc);
	OBOS_KernelAllocator = (allocator_info*)&kalloc;
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
	InitializeHPET();
	Core_SchedulerIRQ = Core_IrqObjectAllocate(&status);
	if (obos_is_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
	status = Core_IrqObjectInitializeIRQL(Core_SchedulerIRQ, IRQL_DISPATCH, false, true);
	if (obos_is_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
	Core_SchedulerIRQ->handler = Arch_SchedulerIRQHandlerEntry;
	Core_SchedulerIRQ->handlerUserdata = nullptr;
	Core_SchedulerIRQ->irqChecker = nullptr;
	Core_SchedulerIRQ->irqCheckerUserdata = nullptr;
	// Hopefully this won't cause trouble.
	Core_SchedulerIRQ->choseVector = true;
	Core_SchedulerIRQ->vector->nIRQsWithChosenID = 1;
	ipi_lapic_info target = {
		.isShorthand = true,
		.info = {
			.shorthand = LAPIC_DESTINATION_SHORTHAND_ALL,
		}
	};
	ipi_vector_info vector = {
		.deliveryMode = LAPIC_DELIVERY_MODE_FIXED,
		.info.vector = Core_SchedulerIRQ->vector->id + 0x20
	};
	Core_LowerIrql(oldIrql);
	Arch_LAPICSendIPI(target, vector);
	while (nCPUsWithInitializedTimer != Core_CpuCount)
		pause();
	OBOS_Debug("%s: Initializing IOAPICs.\n", __func__);
	if (obos_is_error(status = Arch_InitializeIOAPICs()))
		OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not initialize I/O APICs. Status: %d\n", status);
	OBOS_Debug("%s: Initializing timer interface.\n", __func__);
	Core_InitializeTimerInterface();
	OBOS_Debug("%s: Initializing VMM.\n", __func__);
	Arch_InitializeInitialSwapDevice(&swap, (void*)Arch_InitialSwapBuffer->address, Arch_InitialSwapBuffer->size);
	Mm_SwapProvider = &swap;
	Mm_Initialize();
	if (Arch_Framebuffer->physical_address)
	{
		// for (volatile bool b = true; b;)
		// 	;
		OBOS_Debug("Mapping framebuffer as Write-Combining.\n");
		size_t size = (Arch_Framebuffer->height*Arch_Framebuffer->pitch + OBOS_HUGE_PAGE_SIZE - 1) & ~(OBOS_HUGE_PAGE_SIZE - 1);
		void* base_ = Mm_VirtualMemoryAlloc(&Mm_KernelContext, (void*)0xffffa00000000000, size, 0, VMA_FLAGS_NON_PAGED | VMA_FLAGS_HINT | VMA_FLAGS_HUGE_PAGE, nullptr);
		uintptr_t base = (uintptr_t)base_;
		if (base)
		{
			// We got memory for the framebuffer.
			// Now modify the physical pages
			page what;
			memzero(&what, sizeof(what));
			what.addr = base;

			uintptr_t offset = 0;
			page* baseNode = RB_FIND(page_tree, &Mm_KernelContext.pages, &what); 
			OBOS_ASSERT(baseNode);
			page* curr = nullptr;
			extern obos_status Arch_MapHugePage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
			for (uintptr_t addr = base; addr < (base + size); addr += offset)
			{
				what.addr = addr;
				if (addr == base)
					curr = baseNode;
				else
					curr = RB_NEXT(page_tree, &ctx->pages, curr);
				if (!curr || curr->addr != addr)
					OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find page node at address 0x%p.\n", addr);
				uintptr_t oldPhys = 0, phys = Arch_Framebuffer->physical_address + (addr-base);
				OBOSS_GetPagePhysicalAddress((void*)curr->addr, &oldPhys);
				// Present,Write,XD,Write-Combining (PAT: 0b110)
				Arch_MapHugePage(Mm_KernelContext.pt, (void*)addr, phys, BIT_TYPE(0, UL)|BIT_TYPE(1, UL)|BIT_TYPE(63, UL)|BIT_TYPE(4, UL)|BIT_TYPE(12, UL));
				offset = curr->prot.huge_page ? OBOS_HUGE_PAGE_SIZE : OBOS_PAGE_SIZE;
				OBOSS_FreePhysicalPages(oldPhys, offset);
			}
		}
		OBOS_TextRendererState.fb.backbuffer_base = Mm_VirtualMemoryAlloc(
			&Mm_KernelContext, 
			(void*)(0xffffa00000000000+size), size, 
			0, VMA_FLAGS_NON_PAGED | VMA_FLAGS_HINT | VMA_FLAGS_HUGE_PAGE | VMA_FLAGS_GUARD_PAGE, 
			nullptr);
		memcpy(OBOS_TextRendererState.fb.backbuffer_base, OBOS_TextRendererState.fb.base, OBOS_TextRendererState.fb.height*OBOS_TextRendererState.fb.pitch);
		OBOS_TextRendererState.fb.base = base_;
		OBOS_TextRendererState.fb.modified_line_bitmap = OBOS_KernelAllocator->ZeroAllocate(
			OBOS_KernelAllocator,
			get_line_bitmap_size(OBOS_TextRendererState.fb.height),
			sizeof(uint32_t),
			nullptr
		);
	}
	OBOS_Debug("%s: Initializing uACPI\n", __func__);
#define verify_status(st, in) \
if (st != UACPI_STATUS_OK)\
	OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "uACPI Failed in %s! Status code: %d, error message: %s\n", #in, st, uacpi_status_to_string(st));
	uintptr_t rsdp = 0;
#ifdef __x86_64__
	rsdp = Arch_LdrPlatformInfo->acpi_rsdp_address;
#endif
	uacpi_init_params params = {
		rsdp,
		UACPI_LOG_INFO,
		0
	};
	uacpi_status st = uacpi_initialize(&params);
	verify_status(st, uacpi_initialize);

	st = uacpi_namespace_load();
	verify_status(st, uacpi_namespace_load);

	st = uacpi_namespace_initialize();
	verify_status(st, uacpi_namespace_initialize);
	
	uacpi_status ret = uacpi_install_fixed_event_handler(
        UACPI_FIXED_EVENT_POWER_BUTTON,
	handle_power_button, UACPI_NULL
	);

	st = uacpi_finalize_gpe_initialization();
	verify_status(st, uacpi_finalize_gpe_initialization);

    Arch_IOAPICMaskIRQ(9, false);
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
	OBOS_Debug("Loading test driver.\n");
	void* driver_bin = (void*)Arch_UARTDriver->address;
	size_t sizeof_driver_bin = Arch_UARTDriver->size;
	driver_header header;
	status = Drv_LoadDriverHeader(driver_bin, sizeof_driver_bin, &header);
	if (obos_is_error(status))
		OBOS_Error("Could not load test driver #1. Status: %d.\n", status);
	driver_header_node node = {.data = &header};
	driver_header_list list = {.head=&node,.tail=&node,.nNodes=1};
	driver_header_list toLoad;
	status = Drv_PnpDetectDrivers(list, &toLoad);
	if (obos_is_error(status))
		OBOS_Error("PnP failed. Status: %d.\n", status);
	driver_id* drv1 = nullptr;
	for (driver_header_node* node = toLoad.head; node; )
	{
		OBOS_Debug("Loading driver '%s'.\n", node->data->driverName);
		if (&header == node->data)
			drv1 = Drv_LoadDriver(driver_bin, sizeof_driver_bin, nullptr);

		node = node->next;
	}

	thread* main = nullptr;
	Drv_StartDriver(drv1, &main);
	while (!(main->flags & THREAD_FLAGS_DIED))
		Core_Yield();
	dev_desc connection = 0;
    status =
    drv1->header.ftable.ioctl(6, /* IOCTL_OPEN_SERIAL_CONNECTION */0, 
        1,
        115200,
        /* EIGHT_DATABITS */ 3,
        /* ONE_STOPBIT */ 0,
        /* PARITYBIT_NONE */ 0,
        &connection
    );
	if (obos_is_error(status))
	{
		OBOS_Error("Could not open COM1. Status: %d.\n", status);
		goto done;
	}
	char ch = 0;
	size_t nRead = 0;
	while(1)
	{
		status = drv1->header.ftable.read_sync(connection, &ch, 1, 0, &nRead);
		if (nRead != 1)
			continue;
		if (obos_is_error(status))
		{
			OBOS_Error("Could not read from COM1. Status: %d.\n", status);
			goto done;
		}
		status = drv1->header.ftable.write_sync(connection, &ch, 1, 0, &nRead);
		if (obos_is_error(status))
		{
			OBOS_Error("Could not write to COM1. Status: %d.\n", status);
			goto done;
		}
	}
	OBOS_Log("%s: Done early boot.\n", __func__);
	done:
	Core_ExitCurrentThread();

}