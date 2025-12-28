/*
 * src/uHDA/kernel_api.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <driver_interface/pci.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>

#include <locks/spinlock.h>

#include <allocators/base.h>

#include <mm/context.h>
#include <mm/alloc.h>
#include <mm/pmm.h>

#include <uhda/kernel_api.h>
#include <uhda/types.h>

#if OBOS_IRQL_COUNT == 16
#	define IRQL_UHDA (2)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_UHDA (1)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_UHDA (1)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_UHDA (0)
#else
#	error Funny business.
#endif

UhdaStatus uhda_kernel_pci_read(void* dev_ptr, uint8_t offset, uint8_t size, uint32_t* res)
{
    pci_device* dev = dev_ptr;
    if (!dev)
        return UHDA_STATUS_NO_MEMORY;
    uint64_t val = 0;
    DrvS_ReadPCIRegister(dev->location, offset, size, &val);
    *res = val;
    return UHDA_STATUS_SUCCESS;
}

UhdaStatus uhda_kernel_pci_write(void* dev_ptr, uint8_t offset, uint8_t size, uint32_t value)
{
    pci_device* dev = dev_ptr;
    if (!dev)
        return UHDA_STATUS_NO_MEMORY;
    DrvS_WritePCIRegister(dev->location, offset, size, value);
    return UHDA_STATUS_SUCCESS;
}

static void bootstrap_irq_handler_uhda(irq* i, interrupt_frame* f, void* udata, irql oldIrql)
{
    OBOS_UNUSED(i && f && oldIrql);
    uintptr_t *user = udata;
    UhdaIrqHandlerFn cb = (void*)user[0];
    cb((void*)user[1]);
}
static bool no_irq(OBOS_MAYBE_UNUSED irq* i, OBOS_MAYBE_UNUSED void* u)
{ return false; }

UhdaStatus uhda_kernel_pci_allocate_irq(
	void* dev_ptr,
	UhdaIrqHint hint,
	UhdaIrqHandlerFn fn,
	void* arg,
	void** opaque_irq)
{
    pci_device* dev = dev_ptr;
    if (!dev)
        return UHDA_STATUS_NO_MEMORY;
    OBOS_UNUSED(hint);

    irq* isr = Core_IrqObjectAllocate(nullptr);
    OBOS_ENSURE(isr);

    uintptr_t *userdata = ZeroAllocate(OBOS_NonPagedPoolAllocator, 2, sizeof(uintptr_t), nullptr);
    userdata[0] = (uintptr_t)fn;
    userdata[1] = (uintptr_t)arg;

    isr->handlerUserdata = userdata;
    isr->handler = bootstrap_irq_handler_uhda;
    isr->irqChecker = no_irq;
    Core_IrqObjectInitializeIRQL(isr, IRQL_UHDA, false, true);

    pci_resource* irq_res = nullptr;

    for (pci_resource* res = LIST_GET_HEAD(pci_resource_list, &dev->resources); res; )
    {
        if (res->type == PCI_RESOURCE_IRQ)
        {
            irq_res = res;
            break;
        }

        res = LIST_GET_NEXT(pci_resource_list, &dev->resources, res);
    }

    irq_res->irq->masked = true;
    irq_res->irq->irq = isr;
    Drv_PCISetResource(irq_res);
    if (!isr->irqChecker)
        isr->irqChecker = no_irq;
    isr->handlerUserdata = userdata;
    isr->handler = bootstrap_irq_handler_uhda;

    *opaque_irq = irq_res;
    return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_deallocate_irq(void* pci_device, void* opaque_irq)
{
    pci_resource* res = opaque_irq;
    OBOS_ENSURE (res->type == PCI_RESOURCE_IRQ);
    res->irq->masked = false;
    res->irq->irq = nullptr;
    Drv_PCISetResource(res);
    if (!res->irq->irq->irqChecker)
        res->irq->irq->irqChecker = no_irq;
}

/*
 * Enables or disables a previously allocated PCI irq for the device.
 */
void uhda_kernel_pci_enable_irq(void* pci_device, void* opaque_irq, bool enable)
{
    OBOS_UNUSED(pci_device);
    pci_resource* res = opaque_irq;
    OBOS_ENSURE (res->type == PCI_RESOURCE_IRQ);
    res->irq->masked = !enable;
    Drv_PCISetResource(res);
    if (!res->irq->irq->irqChecker)
        res->irq->irq->irqChecker = no_irq;
}

static void* map_registers(uintptr_t phys, size_t size, bool uc)
{
    size_t phys_page_offset = (phys % OBOS_PAGE_SIZE);
    phys -= phys_page_offset;
    size = size + (OBOS_PAGE_SIZE - (size % OBOS_PAGE_SIZE));
    size += phys_page_offset;
    obos_status status = OBOS_STATUS_SUCCESS;
    // void* virt = Mm_VirtualMemoryAlloc(
    //     &Mm_KernelContext, 
    //     nullptr, size,
    //     uc ? OBOS_PROTECTION_CACHE_DISABLE : 0, VMA_FLAGS_NON_PAGED,
    //     nullptr, 
    //     &status);
    irql oldIrql = Core_SpinlockAcquire(&Mm_KernelContext.lock);
    void* virt = MmH_FindAvailableAddress(&Mm_KernelContext, size, 0, &status);
    if (obos_is_error(status))
    {
        OBOS_Error("%s: Status %d\n", __func__, status);
        OBOS_ENSURE(virt);
    }
    page_range* rng = ZeroAllocate(Mm_Allocator, 1, sizeof(page_range), nullptr);
    rng->size = size;
    rng->virt = (uintptr_t)virt;
    rng->ctx = &Mm_KernelContext;
    rng->prot.present = true;
    rng->prot.rw = true;
    rng->prot.ro = false;
    rng->prot.huge_page = false;
    rng->prot.executable = false;
    rng->prot.user = false;
    rng->pageable = false;
    RB_INSERT(page_tree, &Mm_KernelContext.pages, rng);
    for (uintptr_t offset = 0; offset < size; offset += OBOS_PAGE_SIZE)
    {
        page_info page = {.virt=offset+(uintptr_t)virt};
        page.prot = rng->prot;
        page.phys = phys+offset;
        MmS_SetPageMapping(Mm_KernelContext.pt, &page, phys + offset, false);
    }
    Core_SpinlockRelease(&Mm_KernelContext.lock, oldIrql);
    Drv_TLBShootdown(Mm_KernelContext.pt, (uintptr_t)virt, size);
    return virt+phys_page_offset;
}

UhdaStatus uhda_kernel_pci_map_bar(void* dev_ptr, uint32_t bar, void** virt)
{
    pci_device* dev = dev_ptr;
    if (!dev)
        return UHDA_STATUS_NO_MEMORY;

    pci_resource* bar_res = nullptr;

    for (pci_resource* res = LIST_GET_HEAD(pci_resource_list, &dev->resources); res; )
    {
        if (res->type == PCI_RESOURCE_BAR && res->bar->idx == bar)
        {
            bar_res = res;
            break;
        }

        res = LIST_GET_NEXT(pci_resource_list, &dev->resources, res);
    }

    if (bar_res->bar->type == PCI_BARIO)
        return UHDA_STATUS_UNSUPPORTED;

    *virt = map_registers(bar_res->bar->phys, bar_res->bar->size, true);
    return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_pci_unmap_bar(void* pci_device, uint32_t bar, void* virt) 
{
    OBOS_UNUSED(pci_device && bar && virt);
}

void* uhda_kernel_malloc(size_t size)
{
    return Allocate(OBOS_NonPagedPoolAllocator, size, nullptr);
}

void uhda_kernel_free(void* ptr, size_t size)
{
    Free(OBOS_NonPagedPoolAllocator, ptr, size);
}

void uhda_kernel_delay(uint32_t microseconds)
{
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(microseconds);
    while (CoreS_GetTimerTick() < deadline)
        OBOSS_SpinlockHint();
    return;
}

void uhda_kernel_log(const char* str)
{
    OBOS_Log("UHDA: %s", str);
}

UhdaStatus uhda_kernel_allocate_physical(size_t size, uintptr_t* res)
{
    size_t nPages = size / OBOS_PAGE_SIZE;
    if (size % OBOS_PAGE_SIZE)
        nPages++;
    *res = Mm_AllocatePhysicalPages(nPages, 1, nullptr);
    return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_deallocate_physical(uintptr_t phys, size_t size)
{
    size_t nPages = size / OBOS_PAGE_SIZE;
    if (size % OBOS_PAGE_SIZE)
        nPages++;
    Mm_FreePhysicalPages(phys, nPages);
}

UhdaStatus uhda_kernel_map(uintptr_t phys, size_t size, void** virt)
{
    *virt = map_registers(phys, size, true);
    return UHDA_STATUS_SUCCESS;
}

// TODO: uhda_kernel_unmap
void uhda_kernel_unmap(void* virt, size_t size)
{
    OBOS_UNUSED(virt && size);
}

UhdaStatus uhda_kernel_create_spinlock(void** spinlock)
{
    *spinlock = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(struct spinlock), nullptr);
    return UHDA_STATUS_SUCCESS;
}

void uhda_kernel_free_spinlock(void* spinlock)
{
    Free(OBOS_NonPagedPoolAllocator, spinlock, sizeof(struct spinlock));
}

UhdaIrqState uhda_kernel_lock_spinlock(void* spinlock) { return Core_SpinlockAcquireExplicit(spinlock, IRQL_UHDA, true); }

void uhda_kernel_unlock_spinlock(void* spinlock, UhdaIrqState irq_state)
{
    Core_SpinlockRelease(spinlock, irq_state);
}
