/*
 * uACPI/kernel_api.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
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

#include <locks/spinlock.h>
#include <locks/mutex.h>

#include <uacpi_libc.h>

#include <uacpi/status.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>
#include <irq/dpc.h>

#include <allocators/base.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/bare_map.h>

#include <driver_interface/pci.h>

#ifdef __x86_64__
#include <arch/x86_64/pmm.h>
#include <arch/x86_64/asm_helpers.h>
#include <arch/x86_64/lapic.h>
#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/boot_info.h>
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
void spin_hung()
{
    // Use to report a lock hanging.
}

uacpi_status uacpi_kernel_io_read8(
    uacpi_handle hnd, uacpi_size offset, uacpi_u8 *out_value
)
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    *out_value = inb(port+offset);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(
    uacpi_handle hnd, uacpi_size offset, uacpi_u16 *out_value
)
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    *out_value = inw(port+offset);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(
    uacpi_handle hnd, uacpi_size offset, uacpi_u32 *out_value
) 
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    *out_value = ind(port+offset);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(
    uacpi_handle hnd, uacpi_size offset, uacpi_u8 in_value
)
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    outb(port+offset, in_value);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(
    uacpi_handle hnd, uacpi_size offset, uacpi_u16 in_value
)
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    outw(port+offset, in_value);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(
    uacpi_handle hnd, uacpi_size offset, uacpi_u32 in_value
)
{
    uintptr_t port = (uintptr_t)hnd;
#if defined(__x86_64__) || defined(__i386__)
    outd(port+offset, in_value);
#endif
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle)
{
    *out_handle = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(uacpi_pci_address), nullptr);
    memcpy(*out_handle, &address, sizeof(address));
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle hnd)
{
    Free(OBOS_NonPagedPoolAllocator, hnd, sizeof(uacpi_pci_address));
}

uacpi_status uacpi_kernel_pci_read8(
    uacpi_handle device, uacpi_size offset, uacpi_u8 *value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    uint64_t val = 0;
    obos_status status = DrvS_ReadPCIRegister(loc, offset, 1, &val);
    *value = val;
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_read16(
    uacpi_handle device, uacpi_size offset, uacpi_u16 *value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    uint64_t val = 0;
    obos_status status = DrvS_ReadPCIRegister(loc, offset, 2, &val);
    *value = val;
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_read32(
    uacpi_handle device, uacpi_size offset, uacpi_u32 *value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    uint64_t val = 0;
    obos_status status = DrvS_ReadPCIRegister(loc, offset, 4, &val);
    *value = val;
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_write8(
    uacpi_handle device, uacpi_size offset, uacpi_u8 value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    obos_status status = DrvS_WritePCIRegister(loc, offset, 1, value);
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_write16(
    uacpi_handle device, uacpi_size offset, uacpi_u16 value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    obos_status status = DrvS_WritePCIRegister(loc, offset, 2, value);
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_write32(
    uacpi_handle device, uacpi_size offset, uacpi_u32 value
)
{
    uacpi_pci_address* address = device;
    if (address->segment)
        return UACPI_STATUS_UNIMPLEMENTED;
    pci_device_location loc = {
        .bus = address->bus,
        .slot = address->device,
        .function = address->function,
    };
    obos_status status = DrvS_WritePCIRegister(loc, offset, 4, value);
    return status == OBOS_STATUS_SUCCESS ? UACPI_STATUS_OK : UACPI_STATUS_INVALID_ARGUMENT;
}

uacpi_status uacpi_kernel_pci_read(
    uacpi_handle device, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 *value
)
{
    uacpi_pci_address* address = device;
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
    uacpi_handle device, uacpi_size offset,
    uacpi_u8 byte_width, uacpi_u64 value
)
{
    uacpi_pci_address* address = device;
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

    void* ret = Allocate(OBOS_NonPagedPoolAllocator, size, nullptr);
    // void* ret = OBOS_BasicMMAllocatePages(size, nullptr);
    if (!ret)
        OBOS_Warning("%s: Allocation of 0x%lx bytes failed.\n", __func__, size);
    /*else
        OBOS_Debug("Allocated %lu bytes at 0x%p\n", size, ret);*/
    return ret;
}

void* uacpi_kernel_alloc_zeroed(uacpi_size count)
{
    return memzero(uacpi_kernel_alloc(count), count);
}

void uacpi_kernel_free(void* mem, size_t sz)
{
    if (!mem)
        return;
    // logger::debug("Attempt free of 0x%p\n", mem);
    // if (!s_uACPIAllocatorInitialized)
        // logger::panic(nullptr, "Function %s, line %d: free before uACPI allocator is initialized detected. This is a bug, please report in some way.\n", 
        // __func__, __LINE__);
    Free(OBOS_NonPagedPoolAllocator, mem, sz);
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
    if (OBOS_GetLogLevel() == LOG_LEVEL_NONE)
        return;
    const char* prefix = "UNKNOWN";
    color c = 0;
    switch (level)
    {
    case UACPI_LOG_DEBUG:
        prefix = "DEBUG";
        c = OBOS_LogLevelToColor[LOG_LEVEL_DEBUG];
        break;
    case UACPI_LOG_TRACE:
        c = OBOS_LogLevelToColor[LOG_LEVEL_DEBUG];
        prefix = "TRACE";
        break;
    case UACPI_LOG_INFO:
        c = OBOS_LogLevelToColor[LOG_LEVEL_LOG];
        prefix = "INFO";
        break;
    case UACPI_LOG_WARN:
        c = OBOS_LogLevelToColor[LOG_LEVEL_WARNING];
        prefix = "WARN";
        break;
    case UACPI_LOG_ERROR:
        c = OBOS_LogLevelToColor[LOG_LEVEL_ERROR];
        prefix = "ERROR";
        break;
    default:
        break;
    }
    OBOS_SetColor(c);
    printf("[uACPI][%s]: ", prefix);
    vprintf(format, list);
    OBOS_ResetColor();
}

timer_tick CoreS_GetNativeTimerTick();
timer_tick CoreS_GetNativeTimerFrequency();
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    static uint64_t cached_rate = 0;
    // NOTE: If our frequency is greater than 1 GHZ, we get zero for our rate.
    if (obos_expect(!cached_rate, false))
    {
        cached_rate = (1*1000000000)/CoreS_GetNativeTimerFrequency();
        if (!cached_rate)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "uACPI: Conversion from a native timer tick to NS failed.\nNative timer frequency was greater than 1GHZ, which is unsupported. This is a bug, report it.\n");
    }
    return CoreS_GetNativeTimerTick() * cached_rate;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size)
{
#ifdef __x86_64__
    return Arch_MapToHHDM(addr);
#endif
}
void uacpi_kernel_unmap(void* b, uacpi_size s)
{ OBOS_UNUSED(b); OBOS_UNUSED(s); /* Does nothing. */}

uacpi_handle uacpi_kernel_create_spinlock(void)
{
    return ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(spinlock), nullptr);
}
void uacpi_kernel_free_spinlock(uacpi_handle hnd)
{
    Free(OBOS_NonPagedPoolAllocator, hnd, sizeof(spinlock));
}

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle hnd)
{
    spinlock* lock = (spinlock*)hnd;
    // OBOS_Debug("spinlock %p acquire (attempt)\n", hnd);
    irql r = Core_SpinlockAcquire(lock);
    // OBOS_Debug("spinlock %p acquire (acquired)\n", hnd);
    return r;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle hnd, uacpi_cpu_flags oldIrql)
{
    spinlock* lock = (spinlock*)hnd;
    Core_SpinlockRelease(lock, oldIrql);
    // OBOS_Debug("spinlock %p release\n", hnd);
}

uacpi_handle uacpi_kernel_create_event(void)
{
    return ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(size_t), nullptr);
}

void uacpi_kernel_free_event(uacpi_handle e)
{
    Free(OBOS_NonPagedPoolAllocator, e, sizeof(size_t));
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle _e, uacpi_u16 t)
{
    volatile size_t* e = (size_t*)_e;
    // OBOS_Debug("wait for event %p start (t: 0x%04x)\n", e, t);
    if (t == 0xffff)
    {
        while (*e > 0);
        *e -= 1;
        // OBOS_Debug("wait for event %p end (t: 0x%04x)\n", e, t);
        return UACPI_TRUE;
    }
    t *= 4;
    uint64_t wakeTime = CoreS_GetTimerTick() + t;
    while (*e > 0 && CoreS_GetTimerTick() >= wakeTime);
    bool ret = *e <= 0;
    *e -= ret;
    // OBOS_Debug("wait for event %p end (t: 0x%04x)\n", e, t);
    return ret;
}

void uacpi_kernel_signal_event(uacpi_handle _e)
{
    volatile size_t* e = (size_t*)_e;
    // OBOS_Debug("signaled event %p\n", e);
    __atomic_fetch_add(e, 1, __ATOMIC_SEQ_CST);
}
void uacpi_kernel_reset_event(uacpi_handle _e)
{
    volatile size_t* e = (size_t*)_e;
    // OBOS_Debug("reset event %p\n", e);
    __atomic_store_n(e, 0, __ATOMIC_SEQ_CST);
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
{
#if defined(__x86_64__) || defined(__i386__)
    if (base > 0xffff)
        return UACPI_STATUS_INVALID_ARGUMENT;
#endif
    OBOS_UNUSED(len);
    *out_handle = (uacpi_handle)base;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle)
{
    OBOS_UNUSED(handle);
}

uacpi_handle uacpi_kernel_create_mutex(void)
{
    mutex* mut = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(mutex), nullptr);
    *mut = MUTEX_INITIALIZE();
    return (uacpi_handle)mut;
}

void uacpi_kernel_free_mutex(uacpi_handle hnd)
{
    Free(OBOS_NonPagedPoolAllocator, hnd, sizeof(struct mutex));
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle hnd, uacpi_u16 t)
{
    OBOS_UNUSED(t);
    mutex *mut = hnd;
    // OBOS_Debug("mutex %p acquire (attempt) (t: 0x%04x)\n", hnd, t);
    if (t)
        Core_MutexAcquire(mut);
    else
        Core_MutexTryAcquire(mut);
    // OBOS_Debug("mutex %p acquire (acquired) (t: 0x%04x)\n", hnd, t);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle hnd)
{
    mutex *mut = (mutex*)hnd;
    Core_MutexRelease(mut);
    // OBOS_Debug("mutex %p release\n", hnd);
}

uacpi_thread_id uacpi_kernel_get_thread_id()
{
    return (uacpi_thread_id)Core_GetCurrentThread();
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request* req)
{
    switch (req->type)
    {
    case UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT:
        break;
    case UACPI_FIRMWARE_REQUEST_TYPE_FATAL:
        OBOS_Debug("Firmware requested fatal error. Panicking.\n");
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR,
            "Your bios fucked up, so now you have to deal with the consequences, also known as possible data loss. Firmware Error Code: 0x%016x, argument: %016lx\n",
            req->fatal.code, req->fatal.arg);
        break;
    default:
        break;
    }
    return UACPI_STATUS_OK;
}
uint64_t CoreS_TimerTickToNS(timer_tick tp);
void uacpi_kernel_stall(uacpi_u8 usec)
{
    uint64_t ns = usec*1000;
    uint64_t deadline = CoreS_TimerTickToNS(CoreS_GetTimerTick()) + ns;
    while (CoreS_TimerTickToNS(CoreS_GetTimerTick()) < deadline)
        OBOSS_SpinlockHint();
}
void uacpi_kernel_sleep(uacpi_u64 msec)
{
    uint64_t ns = msec*1000000;
    uint64_t deadline = CoreS_TimerTickToNS(CoreS_GetTimerTick()) + ns;
    while (CoreS_TimerTickToNS(CoreS_GetTimerTick()) < deadline)
        OBOSS_SpinlockHint();
}
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr* out)
{
    uintptr_t rsdp = 0;
#ifdef __x86_64__
    rsdp = Arch_LdrPlatformInfo->acpi_rsdp_address;
#endif
    *out = rsdp;
    return UACPI_STATUS_OK;
}

static void bootstrap_irq_handler(irq* i, interrupt_frame* frame, void* udata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(oldIrql);
    uacpi_handle ctx = *((void**)udata);
    uacpi_interrupt_handler handler = (uacpi_interrupt_handler)((void**)udata)[1];
    //printf("%s calling 0x%p(0x%p)\n", __func__, handler, ctx);
    handler(ctx);
}

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler handler, uacpi_handle ctx,
    uacpi_handle *out_irq_handle
)
{
    struct irq* irqHnd = Core_IrqObjectAllocate(nullptr);
    obos_status status = Core_IrqObjectInitializeIRQL(irqHnd, IRQL_GPE, false, true);
    if (obos_is_error(status))
    {
        OBOS_Error("%s: Could not initialize IRQ object. Status: %d.\n", __func__, status);
        return UACPI_STATUS_INVALID_ARGUMENT;
    }
    uintptr_t *udata = ZeroAllocate(OBOS_NonPagedPoolAllocator, 2, sizeof(uintptr_t), nullptr);
    udata[0] = (uintptr_t)ctx;
    udata[1] = (uintptr_t)handler;
    irqHnd->handler = bootstrap_irq_handler;
    irqHnd->handlerUserdata = udata;
    irql oldIrql = Core_RaiseIrql(IRQL_GPE);
#if defined(__x86_64__)
    if (Arch_IOAPICMapIRQToVector(irq, irqHnd->vector->id+0x20, PolarityActiveHigh, TriggerModeLevelSensitive) != OBOS_STATUS_SUCCESS)
        return UACPI_STATUS_INTERNAL_ERROR;
    Arch_IOAPICMaskIRQ(irq, false);
#endif
    Core_LowerIrql(oldIrql);
    *out_irq_handle = irqHnd;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(
    uacpi_interrupt_handler unused, uacpi_handle irq_handle
)
{
    OBOS_UNUSED(unused);
    struct irq* irqHnd = (irq*)irq_handle;
    Free(OBOS_NonPagedPoolAllocator, irqHnd->handlerUserdata, sizeof(uintptr_t)*2);
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
    CoreH_FreeDPC(dpc, true);
    Free(OBOS_NonPagedPoolAllocator, work, sizeof(*work));
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler cb, uacpi_handle ctx)
{
    if (!s_isWorkQueueLockInit)
        s_workQueueLock = Core_SpinlockCreate();
    // Make the work object.
    uacpi_work* work = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(uacpi_work), nullptr);
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
        if (spin++ > 1000000)
			spin_hung();
        spinlock_hint();
    }
    return UACPI_STATUS_OK;
}

#endif
// So we use the uACPI stdlib in some places in the kernel...

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
    return strnlen(src, maxcnt);
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
