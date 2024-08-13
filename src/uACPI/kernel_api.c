/*
	libs/uACPI/kernel_api.cpp
	
	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <stdatomic.h>

#include <stdint.h>
#include <uacpi/kernel_api.h>

#include <scheduler/schedule.h>
#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <irq/dpc.h>

#include <locks/spinlock.h>

#include <uacpi_libc.h>

#include <uacpi/status.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>

#include <allocators/base.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <driver_interface/pci.h>

#ifdef __x86_64__
#include <arch/x86_64/pmm.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/ioapic.h>
#endif

#if OBOS_ARCHITECTURE_HAS_ACPI
static bool isPower2(uint64_t num)
{
	int popcount = __builtin_popcount(num);
	return popcount == 1 /* A power of two only ever has one bit set. */;
}
#if defined(__i386__) || defined(__x86_64__)
#	define spinlock_hint() asm volatile("pause")
#else
#	error Unknown arch.
#endif

// uACPI kernel api.
uacpi_status uacpi_kernel_raw_memory_read(uacpi_phys_addr address, uacpi_u8 byteWidth, uacpi_u64 *out_value)
{
#ifdef __x86_64__
    void* virt = Arch_MapToHHDM(address);
#endif
    switch (byteWidth)
    {
        case 1: *out_value = *(volatile  uint8_t*)virt; break;
        case 2: *out_value = *(volatile uint16_t*)virt; break;
        case 4: *out_value = *(volatile uint32_t*)virt; break;
        case 8: *out_value = *(volatile uint64_t*)virt; break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_raw_memory_write(uacpi_phys_addr address, uacpi_u8 byteWidth, uacpi_u64 in_value)
{
#ifdef __x86_64__
    void* virt = Arch_MapToHHDM(address);
#endif
    switch (byteWidth)
    {
        case 1: *(volatile  uint8_t*)virt = in_value; break;
        case 2: *(volatile uint16_t*)virt = in_value; break;
        case 4: *(volatile uint32_t*)virt = in_value; break;
        case 8: *(volatile uint64_t*)virt = in_value; break;
        default: return UACPI_STATUS_INVALID_ARGUMENT;
    }
    return UACPI_STATUS_OK;
}
#ifdef __x86_64__
    static uint8_t raw_io_readB_impl(uacpi_io_addr address) { return inb(address); };
    static uint16_t raw_io_readW_impl(uacpi_io_addr address) { return inw(address); };
    static uint32_t raw_io_readD_impl(uacpi_io_addr address) { return ind(address); };
    static uint8_t (*raw_io_readB)(uacpi_io_addr address) = raw_io_readB_impl;
    static uint16_t(*raw_io_readW)(uacpi_io_addr address) = raw_io_readW_impl;
    static uint32_t(*raw_io_readD)(uacpi_io_addr address) = raw_io_readD_impl;
    static uint64_t(*raw_io_readQ)(uacpi_io_addr address) = nullptr;
#endif
// struct stack_frame
// {
// 	struct stack_frame* down;
// 	void* rip;
// };
// void spin_hung()
// {
//     OBOS_Warning("Spinlock could be hanging!\n");
//     struct stack_frame* curr = (struct stack_frame*)__builtin_frame_address(0);
//     OBOS_Debug("Stack trace:\n");
//     for (int i = 0; curr; )
//     {
//         OBOS_Debug("\t%i: %p\n", i, curr->rip);
//         curr = curr->down;
//     }
// }
void spin_hung()
{
    // Use to report a lock hanging.
}
uacpi_status uacpi_kernel_raw_io_read(uacpi_io_addr address, uacpi_u8 byteWidth, uacpi_u64 *out_value)
{
    uint8_t (*readB)(uacpi_io_addr address) = raw_io_readB;
    uint16_t(*readW)(uacpi_io_addr address) = raw_io_readW;
    uint32_t(*readD)(uacpi_io_addr address) = raw_io_readD;
    uint64_t(*readQ)(uacpi_io_addr address) = raw_io_readQ;
    if (!isPower2(byteWidth))
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (byteWidth > 8)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_status status = UACPI_STATUS_OK;
    if (byteWidth == 1)
    {
        if (readB)
            *out_value = readB(address);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 2)
    {
        if (readW)
            *out_value = readW(address);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 4)
    {
        if (readD)
            *out_value = readD(address);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 8)
    {
        if (readQ)
            *out_value = readQ(address);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    return status;
}
#ifdef __x86_64__
    static void raw_io_writeB_impl(uacpi_io_addr address, uint8_t val)  { outb(address, val); };
    static void raw_io_writeW_impl(uacpi_io_addr address, uint16_t val) { outw(address, val); };
    static void raw_io_writeD_impl(uacpi_io_addr address, uint32_t val) { outd(address, val); };
    static void(*raw_io_writeB)(uacpi_io_addr address, uint8_t  val) = raw_io_writeB_impl;
    static void(*raw_io_writeW)(uacpi_io_addr address, uint16_t val) = raw_io_writeW_impl;
    static void(*raw_io_writeD)(uacpi_io_addr address, uint32_t val) = raw_io_writeD_impl;
    static void(*raw_io_writeQ)(uacpi_io_addr address, uint64_t val) = nullptr;
#endif
uacpi_status uacpi_kernel_raw_io_write(uacpi_io_addr address, uacpi_u8 byteWidth, uacpi_u64 in_value)
{
    void(*writeB)(uacpi_io_addr address, uint8_t  val) = raw_io_writeB;
    void(*writeW)(uacpi_io_addr address, uint16_t val) = raw_io_writeW;
    void(*writeD)(uacpi_io_addr address, uint32_t val) = raw_io_writeD;
    void(*writeQ)(uacpi_io_addr address, uint64_t val) = raw_io_writeQ;
    if (!isPower2(byteWidth))
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (byteWidth > 8)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_status status = UACPI_STATUS_OK;
    if (byteWidth == 1)
    {
        if (writeB)
            writeB(address, in_value);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 2)
    {
        if (writeW)
            writeW(address, in_value);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 4)
    {
        if (writeD)
            writeD(address, in_value);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    if (byteWidth == 8)
    {
        if (writeQ)
            writeQ(address, in_value);
        else
            status = UACPI_STATUS_INVALID_ARGUMENT;
    }
    return status;
}
uacpi_status uacpi_kernel_pci_read(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value
)
{
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    obos_status status = DrvS_ReadPCIRegister(loc, offset, byte_width, value);
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}
uacpi_status uacpi_kernel_pci_write(
    uacpi_pci_address *address, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
)
{
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    obos_status status = DrvS_WritePCIRegister(loc, offset, byte_width, value);
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}
// static allocators::BasicAllocator s_uACPIAllocator;
// static bool s_uACPIAllocatorInitialized = false;
void* uacpi_kernel_alloc(uacpi_size size)
{
    // logger::debug("Attempting allocation of %lu bytes.\n", size);
    // if (!s_uACPIAllocatorInitialized)
    // {
        // new (&s_uACPIAllocator) allocators::BasicAllocator{};
        // s_uACPIAllocatorInitialized = true;
    // }
    void* ret = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, size, nullptr);
    if (!ret)
        OBOS_Warning("%s: Allocation of 0x%lx bytes failed.\n", __func__, size);
    /*else
        OBOS_Debug("Allocated %lu bytes at 0x%p\n", size, ret);*/
    return ret;
}
void* uacpi_kernel_calloc(uacpi_size count, uacpi_size size)
{
    return memzero(uacpi_kernel_alloc(count * size), count * size);
}
void uacpi_kernel_free(void* mem)
{
    if (!mem)
        return;
    if (mem == (void*)0xffff8000feed50f0)
        asm volatile("nop");
    // logger::debug("Attempt free of 0x%p\n", mem);
    // if (!s_uACPIAllocatorInitialized)
        // logger::panic(nullptr, "Function %s, line %d: free before uACPI allocator is initialized detected. This is a bug, please report in some way.\n", 
        // __func__, __LINE__);
    size_t sz = 0;
    OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, mem, &sz);
    if (sz == SIZE_MAX)
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Function %s, line %d: free of object by uACPI not allocated by uACPI allocator. This is a bug, please report in some way.\n", 
        __func__, __LINE__);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, mem, sz);
    //logger::debug("Freed 0x%p.\n", mem);
}
void uacpi_kernel_log(enum uacpi_log_level level, const char* format, ...)
{
    va_list list;
    va_start(list, format);
    uacpi_kernel_vlog(level, format, list);
    va_end(list);
}
void uacpi_kernel_vlog(enum uacpi_log_level level, const char* format, uacpi_va_list list)
{
    const char* prefix = "UNKNOWN";
    switch (level)
    {
    case UACPI_LOG_TRACE:
        prefix = "TRACE";
        break;
    case UACPI_LOG_INFO:
        prefix = "INFO";
        break;
    case UACPI_LOG_WARN:
        prefix = "WARN";
        break;
    case UACPI_LOG_ERROR:
        prefix = "ERROR";
        break;
    default:
        break;
    }
    printf("uACPI, %s: ", prefix);
    vprintf(format, list);
}
uint64_t CoreS_TimerTickToNS(timer_tick tp);
uacpi_u64 uacpi_kernel_get_ticks(void)
{
    // This value is strictly monotonic, so in the case that the tick counter doesn't change,
    // we need to account for that.
    static uint64_t cached = 0;
    static uint64_t offset = 0;
    if (cached == CoreS_TimerTickToNS(CoreS_GetTimerTick())/100)
        return cached + (++offset);
    cached = CoreS_TimerTickToNS(CoreS_GetTimerTick())/100;
    offset = 0;
    return cached;
}
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size)
{
#ifdef __x86_64__
    return Arch_MapToHHDM(addr);
#endif
}
void uacpi_kernel_unmap(void *, uacpi_size )
{ /* Does nothing. */}
uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sizeof(spinlock), nullptr);
}
void uacpi_kernel_free_spinlock(uacpi_handle hnd)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, hnd, sizeof(spinlock));
}
uacpi_cpu_flags uacpi_kernel_spinlock_lock(uacpi_handle hnd)
{
    spinlock* lock = (spinlock*)hnd;
    return Core_SpinlockAcquire(lock);
}
void uacpi_kernel_spinlock_unlock(uacpi_handle hnd, uacpi_cpu_flags oldIrql)
{
    spinlock* lock = (bool*)hnd;
    Core_SpinlockRelease(lock, oldIrql);
}
uacpi_handle uacpi_kernel_create_event(void)
{
    return OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sizeof(size_t), nullptr);
}
void uacpi_kernel_free_event(uacpi_handle e)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, e, sizeof(size_t));
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle _e, uacpi_u16 t)
{
    volatile size_t* e = (size_t*)_e;
    if (t == 0xffff)
    {
        while (*e > 0);
        return UACPI_TRUE;
    }
    t *= 4;
    uint64_t wakeTime = CoreS_GetTimerTick() + t;
    while (*e > 0 && CoreS_GetTimerTick() >= wakeTime);
    bool ret = *e > 0;
    *e -= ret;
    return ret;
}
void uacpi_kernel_signal_event(uacpi_handle _e)
{
    volatile size_t* e = (size_t*)_e;
    __atomic_fetch_add(e, 1, __ATOMIC_SEQ_CST);
}
void uacpi_kernel_reset_event(uacpi_handle _e)
{
    volatile size_t* e = (size_t*)_e;
    __atomic_store_n(e, 0, __ATOMIC_SEQ_CST);
}
typedef struct io_range
{
    uacpi_io_addr base;
    uacpi_size len;
} io_range;
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
{
    if (base > 0xffff)
        return UACPI_STATUS_INVALID_ARGUMENT;
    io_range* rng = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(io_range), nullptr);
    rng->base = base;
    rng->len = len;
    *out_handle = (uacpi_handle)rng;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, handle, sizeof(io_range));
}
uacpi_status uacpi_kernel_io_read(
    uacpi_handle hnd, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value
)
{
    io_range* rng = (io_range*)hnd;
    if (offset >= rng->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    return uacpi_kernel_raw_io_read(rng->base + offset, byte_width, value);
}
uacpi_status uacpi_kernel_io_write(
    uacpi_handle hnd, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
)
{
    io_range* rng = (io_range*)hnd;
    if (offset >= rng->len)
        return UACPI_STATUS_INVALID_ARGUMENT;
    return uacpi_kernel_raw_io_write(rng->base + offset, byte_width, value);
}
typedef struct mutex
{
    atomic_flag locked;
    // If not nullptr, locked.
    thread* owner;
} mutex;
uacpi_handle uacpi_kernel_create_mutex(void)
{
    return OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sizeof(mutex), nullptr);
}
void uacpi_kernel_free_mutex(uacpi_handle hnd)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, hnd, sizeof(mutex));
}
uacpi_bool uacpi_kernel_acquire_mutex(uacpi_handle hnd, uacpi_u16 t)
{
    mutex *mut = (mutex*)hnd;
    uint64_t wakeTime = 0;
    if (t != 0xffff)
        wakeTime = CoreS_GetTimerTick() + t * 4;
    else
        wakeTime = 0xffffffffffffffff;
    size_t spin = 0;
    while (atomic_flag_test_and_set_explicit(&mut->locked, memory_order_seq_cst))
	{
        if (spin++ == 10000)
            spin_hung();
        spinlock_hint();
    }
    mut->owner = Core_GetCurrentThread();
    return UACPI_TRUE;
}
void uacpi_kernel_release_mutex(uacpi_handle hnd)
{
    mutex *mut = (mutex*)hnd;
    if (mut->owner != Core_GetCurrentThread())
    {
        OBOS_Debug("Failed release of mutex %p. Owner != currentThread\n", hnd);
        return;
    }
	atomic_flag_clear_explicit(&mut->locked, memory_order_seq_cst);
}
uacpi_thread_id uacpi_kernel_get_thread_id()
{
    return Core_GetCurrentThread()->tid;
}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* req)
{
    switch (req->type)
    {
    case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
        break;
    case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR,
            "Your bios fucked up, so now you have to deal with the consequences, also known as possible data loss. Firmware Error Code: 0x%016x, argument: %016lx\n",
            req->fatal.code, req->fatal.arg);
        break;
    default:
        break;
    }
    return UACPI_STATUS_OK;
}
void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t ns = usec*1000;
    uint64_t deadline = CoreS_TimerTickToNS(CoreS_GetTimerTick()) + ns;
    while (CoreS_TimerTickToNS(CoreS_GetTimerTick()) < deadline)
        Core_Yield();
}
void uacpi_kernel_sleep(uacpi_u64 msec)
{
    uint64_t ns = msec*1000000;
    uint64_t deadline = CoreS_TimerTickToNS(CoreS_GetTimerTick()) + ns;
    while (CoreS_TimerTickToNS(CoreS_GetTimerTick()) < deadline)
        Core_Yield();
}

void bootstrap_irq_handler(irq* i, interrupt_frame* frame, void* udata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(oldIrql);
    uacpi_handle ctx = *((void**)udata);
    uacpi_interrupt_handler handler = (uacpi_interrupt_handler)((void**)udata)[1];
    handler(ctx);
}
uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle
)
{
    struct irq* irqHnd = Core_IrqObjectAllocate(nullptr);
    obos_status status = Core_IrqObjectInitializeIRQL(irqHnd, IRQL_DISPATCH, false, true);
    if (obos_is_error(status))
    {
        OBOS_Debug("%s: Could not initialize IRQ object. Status: %d.\n", __func__, status);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uintptr_t *udata = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(uintptr_t), nullptr);
    udata[0] = (uintptr_t)ctx;
    udata[1] = (uintptr_t)handler;
    irqHnd->handler = bootstrap_irq_handler;
    irqHnd->handlerUserdata = udata;
#if defined(__x86_64__)
    if (Arch_IOAPICMapIRQToVector(irq, irqHnd->vector->id+0x20, false, TriggerModeEdgeSensitive) != OBOS_STATUS_SUCCESS)
        return UACPI_STATUS_INTERNAL_ERROR;
    // TODO: Fix issue where some hardware gets infinite GPEs.
    // NOTE(oberrow): It's quite a funny issue, except it's kinda annoying.
    Arch_IOAPICMaskIRQ(irq, /* false */ false);
#endif
    *out_irq_handle = irqHnd;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler unused, uacpi_handle irq_handle
)
{
    OBOS_UNUSED(unused);
    struct irq* irqHnd = (irq*)irq_handle;
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, irqHnd->handlerUserdata, sizeof(uintptr_t)*2);
    Core_IrqObjectFree(irqHnd);
    return UACPI_STATUS_OK;
}	
typedef struct uacpi_work
{
    uacpi_work_type type;
    uacpi_work_handler cb; 
    uacpi_handle ctx;
    dpc* work;
    struct uacpi_work *next, *prev;
} uacpi_work;
static uacpi_work *s_workHead = nullptr, *s_workTail = nullptr;
static size_t s_nWork = 0;
static spinlock s_workQueueLock;
static bool s_isWorkQueueLockInit = false;
static void work_handler(dpc* dpc, void* userdata)
{
    OBOS_UNUSED(dpc);
    uacpi_work* work = (uacpi_work*)userdata;
    work->cb(work->ctx);
    // Remove the work from the queue.
    irql oldIrql = Core_SpinlockAcquire(&s_workQueueLock);
    if (work->next)
        work->next->prev = work->prev;
    if (work->prev)
        work->prev->next = work->next;
    if (s_workTail == work)
        s_workTail = work->prev;
    if (s_workHead == work)
        s_workHead = work->next;
    s_nWork--;
    Core_SpinlockRelease(&s_workQueueLock, oldIrql);
    CoreH_FreeDPC(dpc);
}
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler cb, uacpi_handle ctx)
{
    if (!s_isWorkQueueLockInit)
        s_workQueueLock = Core_SpinlockCreate();
    // Make the work object.
    uacpi_work* work = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(uacpi_work), nullptr);
    work->type = type;
    work->cb = cb; 
    work->ctx = ctx;
    irql oldIrql = Core_SpinlockAcquire(&s_workQueueLock);
    if(!s_workHead)
        s_workHead = work;
    if (s_workTail)
        s_workTail->next = work;
    work->prev = s_workTail;
    s_workTail = work;
    s_nWork++;
    Core_SpinlockRelease(&s_workQueueLock, oldIrql);
    thread_affinity affinity = 0;
    if (type == UACPI_WORK_GPE_EXECUTION)
        affinity = 1;
    else
        affinity = Core_DefaultThreadAffinity;
    work->work = CoreH_AllocateDPC(nullptr);
    work->work->userdata = work;
    CoreH_InitializeDPC(work->work, work_handler, affinity);
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void)
{
	size_t spin = 0;
    while (s_nWork > 0)
    {
        if (spin++ > 10000)
			spin_hung();
        spinlock_hint();
    }
    return UACPI_STATUS_OK;
}

#endif
// So we use the uACPI stdlib in some places in the kernel.

// uacpi_stdlib
void *uacpi_memcpy(void *dest, const void* src, size_t sz)
{
    return memcpy(dest,src,sz);
}
void *uacpi_memset(void *dest, int src, size_t cnt)
{
    return memset(dest, src, cnt);
}
int uacpi_memcmp(const void *src1, const void *src2, size_t cnt)
{
    const uint8_t* b1 = (const uint8_t*)src1;
    const uint8_t* b2 = (const uint8_t*)src2;
    for (size_t i = 0; i < cnt; i++)
        if (b1[i] < b2[i])
            return -1;
        else if (b1[i] > b2[i])
            return 1;
        else
            continue;
    return 0;
}
int uacpi_strncmp(const char *src1, const char *src2, size_t maxcnt)
{
    size_t len1 = uacpi_strnlen(src1, maxcnt);
    size_t len2 = uacpi_strnlen(src2, maxcnt);
    if (len1 < len2)
        return -1;
    else if (len1 > len2)
        return 1;
    return uacpi_memcmp(src1, src2, len1);
}
int uacpi_strcmp(const char *src1, const char *src2)
{
    size_t len1 = uacpi_strlen(src1);
    size_t len2 = uacpi_strlen(src2);
    if (len1 < len2)
        return -1;
    else if (len1 > len2)
        return 1;
    return uacpi_memcmp(src1, src2, len1);
}
void *uacpi_memmove(void *dest, const void* src, size_t len)
{
    if (src == dest)
        return dest;
    // Refactored from https://stackoverflow.com/a/65822606
    uint8_t *dp = (uint8_t *)dest;
    const uint8_t *sp = (uint8_t *)src;
    if(sp < dp && sp + len > dp)
    {
        sp += len;
        dp += len;
        while(len-- > 0)
            *--dp = *--sp;
    }
    else
        while(len-- > 0)
            *dp++ = *sp++;
    return dest;
}
size_t uacpi_strnlen(const char *src, size_t maxcnt)
{
    size_t i = 0;
    for (; i < maxcnt && src[i]; i++);
    return i;
}
size_t uacpi_strlen(const char *src)
{
    return strlen(src);
}
int uacpi_snprintf(char* dest, size_t n, const char* format, ...)
{
    va_list list;
    va_start(list, format);
    int ret = vsnprintf(dest, n, format, list);
    va_end(list);
    return ret;
}