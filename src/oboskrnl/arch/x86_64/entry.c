/*
 * oboskrnl/arch/x86_64/entry.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
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

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/pmm.h>

#include <allocators/basic_allocator.h>

#include <arch/x86_64/boot_info.h>

#include <arch/x86_64/lapic.h>

#include <irq/irq.h>

#include <mm/bare_map.h>

#include <scheduler/process.h>

extern void Arch_InitBootGDT();

static char thr_stack[0x4000];
static char kmain_thr_stack[0x10000];
extern char Arch_InitialISTStack[0x10000];
static thread bsp_idleThread;
static thread_node bsp_idleThreadNode;

static thread kernelMainThread;
static thread_node kernelMainThreadNode;

static cpu_local bsp_cpu;
extern void Arch_IdleTask();
void Arch_CPUInitializeGDT(cpu_local* info, uintptr_t istStack, size_t istStackSize);
void Arch_KernelMainBootstrap();
static void ParseBootContext(struct ultra_boot_context* bcontext);
void pageFaultHandler(interrupt_frame* frame);
void Arch_KernelEntry(struct ultra_boot_context* bcontext)
{
	// This call will ensure the IRQL is at the default IRQL (IRQL_MASKED).
	ParseBootContext(bcontext);
	Core_GetIrql();
	asm("sti");
	OBOS_Debug("%s: Initializing the Boot GDT.\n", __func__);
	Arch_InitBootGDT();
	OBOS_Debug("%s: Initializing the Boot IDT.\n", __func__);
	Arch_RawRegisterInterrupt(0xe, (uintptr_t)pageFaultHandler);
	if (Arch_LdrPlatformInfo->page_table_depth != 4)
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "5-level paging is unsupported by oboskrnl.\n");
	if (!Arch_Framebuffer)
		OBOS_Warning("No framebuffer passed by the framebuffer. All kernel logs will be on port 0xE9.\n");
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
	}
#if OBOS_RELEASE
	OBOS_Log("Booting OBOS %s committed on %s. Build time: %s.\n", GIT_SHA1, GIT_DATE, __DATE__ " " __TIME__);
#endif
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
	OBOS_Debug("Setting up thread context.\n");
	CoreS_SetupThreadContext(&ctx2, (uintptr_t)Arch_KernelMainBootstrap, 0, false, kmain_thr_stack, 0x10000);
	CoreS_SetupThreadContext(&ctx1, (uintptr_t)Arch_IdleTask, 0, false, thr_stack, 0x4000);
	OBOS_Debug("Initializing thread.\n");
	CoreH_ThreadInitialize(&kernelMainThread, THREAD_PRIORITY_NORMAL, 1, &ctx2);
	CoreH_ThreadInitialize(&bsp_idleThread, THREAD_PRIORITY_IDLE, 1, &ctx1);
	kernelMainThread.context.gs_base = (uintptr_t)&bsp_cpu;
	bsp_idleThread.context.gs_base = (uintptr_t)&bsp_cpu;
	OBOS_Debug("Readying threads for execution.\n");
	CoreH_ThreadReadyNode(&kernelMainThread, &kernelMainThreadNode);
	CoreH_ThreadReadyNode(&bsp_idleThread, &bsp_idleThreadNode);
	Core_CpuInfo->idleThread = &bsp_idleThread;
	OBOS_Debug("Initializing CPU-local GDT.\n");
	// Initialize the CPU's GDT.
	Arch_CPUInitializeGDT(&Core_CpuInfo[0], (uintptr_t)Arch_InitialISTStack, sizeof(Arch_InitialISTStack));
	OBOS_Debug("Initializing GS_BASE.\n");
	wrmsr(0xC0000101, (uintptr_t)&Core_CpuInfo[0]);
	Core_CpuInfo[0].currentIrql = Core_GetIrql();
	Core_CpuInfo[0].arch_specific.ist_stack = Arch_InitialISTStack;
	OBOS_Debug("Initializing priority lists.\n");
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
struct ultra_framebuffer* Arch_Framebuffer;
const char* OBOS_KernelCmdLine;
static OBOS_NO_KASAN void ParseBootContext(struct ultra_boot_context* bcontext)
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
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find kernel module in boot context!\nDo you set kernel-as-module to true in the hyper.cfg?\n");
}
extern obos_status Arch_InitializeKernelPageTable();
void pageFaultHandler(interrupt_frame* frame)
{
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
		getCR0(), getCR2(), getCR3(),
		getCR4(), Core_GetIrql(), getEFER()
	);
}
uint64_t random_number();
__asm__(
	"random_number:; rdrand %rax; ret; "
);
allocator_info* OBOS_KernelAllocator;
static basic_allocator kalloc;
void Arch_SMPStartup();
extern uint64_t Arch_FindCounter(uint64_t hz);
atomic_size_t nCPUsWithInitializedTimer;
void Arch_SchedulerIRQHandlerEntry(irq* obj, interrupt_frame* frame, void* userdata, irql oldIrql)
{
	(obj = obj);
	(frame = frame);
	(userdata = userdata);
	(obj = obj);
	if (!CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer)
	{
		Arch_LAPICAddress->lvtTimer = 0x20000 | (Core_SchedulerIRQ->vector->id + 0x20);
		Arch_LAPICAddress->divideConfig = 0b1101;
		Arch_LAPICAddress->initialCount = Arch_FindCounter(Core_SchedulerTimerFrequency);
		OBOS_Debug("Initialized timer for CPU %d.\n", CoreS_GetCPULocalPtr()->id);
		CoreS_GetCPULocalPtr()->arch_specific.initializedSchedulerTimer = true;
		nCPUsWithInitializedTimer++;
	}
	else
		Core_Yield();
}
HPET* Arch_HPETAddress;
uint64_t Arch_HPETFrequency;
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
static OBOS_NO_UBSAN void InitializeHPET()
{
	extern obos_status Arch_MapPage(uintptr_t cr3, void* at_, uintptr_t phys, uintptr_t flags);
	static basicmm_region hpet_region;
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
	Arch_HPETAddress = (HPET*)0xffffffffffffe000;
	Arch_MapPage(getCR3(), Arch_HPETAddress, phys, 0x8000000000000013);
	OBOSH_BasicMMAddRegion(&hpet_region, Arch_HPETAddress, 0x1000);
}
static atomic_size_t nThreadsEntered = 0;
static void scheduler_test(size_t maxPasses)
{
	OBOS_Debug("Entered thread %d (work id %d).\n", Core_GetCurrentThread()->tid, nThreadsEntered++);
	volatile uint8_t* lastPtr = nullptr;
	volatile size_t lastFree = 0;
	for (size_t i = 0; i < maxPasses; i++)
	{
		volatile size_t sz = random_number() % 0x1000 + 1;
		volatile uint8_t *buf = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sz, nullptr);
		OBOS_ASSERT(buf);
		buf[0] = 0xDE;
		buf[sz - 1] = 0xAD;
		if (i - lastFree == 3)
		{
			size_t blkSz = 0;
			OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, lastPtr, &blkSz);
			OBOS_KernelAllocator->Free(OBOS_KernelAllocator, lastPtr, blkSz);
		}
		lastPtr = buf;
	}
	OBOS_Debug("Exiting thread %d.\n", Core_GetCurrentThread()->tid);
	Core_ExitCurrentThread();
}
process* OBOS_KernelProcess;
void Arch_KernelMainBootstrap()
{
	//Core_Yield();
	irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
	OBOS_Debug("%s: Initializing PMM.\n", __func__);
	Arch_InitializePMM();
	OBOS_Debug("%s: Initializing page tables.\n", __func__);
	obos_status status = Arch_InitializeKernelPageTable();
	if (obos_likely_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize page tables. Status: %d.\n", status);
	bsp_idleThread.context.cr3 = getCR3();
	OBOS_Debug("%s: Initializing allocator...\n", __func__);
	OBOSH_ConstructBasicAllocator(&kalloc);
	OBOS_KernelAllocator = (allocator_info*)&kalloc;
	OBOS_Debug("%s: Initializing kernel process.\n", __func__);
	OBOS_KernelProcess = Core_ProcessAllocate(&status);
	if (obos_likely_error(status))
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
	if (obos_likely_error(status = Core_InitializeIRQInterface()))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize irq interface. Status: %d.\n", status);
	OBOS_Debug("%s: Initializing scheduler timer.\n", __func__);
	InitializeHPET();
	Core_SchedulerIRQ = Core_IrqObjectAllocate(&status);
	if (obos_likely_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
	status = Core_IrqObjectInitializeIRQL(Core_SchedulerIRQ, IRQL_DISPATCH, false, true);
	if (obos_likely_error(status))
		OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize the scheduler IRQ. Status: %d.\n", status);
	Core_SchedulerIRQ->handler = Arch_SchedulerIRQHandlerEntry;
	Core_SchedulerIRQ->handlerUserdata = nullptr;
	Core_SchedulerIRQ->irqChecker = nullptr;
	Core_SchedulerIRQ->irqCheckerUserdata = nullptr;
	// Hopefully this won't cause trouble.
	Core_SchedulerIRQ->choseVector = false;
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
	//Arch_PutInterruptOnIST(Core_SchedulerIRQ->vector->id + 0x20, 1);
	Arch_LAPICSendIPI(target, vector);
	while (nCPUsWithInitializedTimer != Core_CpuCount)
		pause();
	OBOS_Debug("%s: Testing the scheduler.\n", __func__);
	for (size_t i = 0; i < 0; i++)
	{
		thread_ctx ctx;
		memzero(&ctx, sizeof(ctx));
		void* stack = OBOS_BasicMMAllocatePages(0x4000, nullptr);
		CoreS_SetupThreadContext(&ctx, (uintptr_t)scheduler_test, (i + 1) * 4000, false, stack, 0x4000);
		thread* thr = CoreH_ThreadAllocate(nullptr);
		if (obos_unlikely_error(CoreH_ThreadInitialize(thr, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx)))
		{
			CoreH_ThreadReady(thr);
			Core_ProcessAppendThread(OBOS_KernelProcess, thr);
		}
		else
			OBOS_Warning("%s: Could not initialize thread.\n", __func__);
	}
	OBOS_Log("%s: Done early boot. Boot required %d KiB of memory to finish.\n", __func__, Arch_TotalPhysicalPagesUsed * 4);
	Core_ExitCurrentThread();
}