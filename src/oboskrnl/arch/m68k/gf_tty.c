/*
 * oboskrnl/arch/m68k/gf_tty.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <scheduler/process.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>
#include <scheduler/thread_context_info.h>

#include <vfs/tty.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <locks/event.h>

#include <allocators/basic_allocator.h>

#include <arch/m68k/goldfish_pic.h>
#include <arch/m68k/boot_info.h>

extern uintptr_t Arch_TTYBase;

struct gf_tty {
    volatile uint32_t put_char;
    volatile const uint32_t bytes_ready;
    volatile uint32_t command;
    volatile uint32_t resv;
    volatile uint32_t data_ptr;
    volatile uint32_t data_len;
    volatile uint32_t data_ptr_high;
};

enum {
    CMD_INT_DISABLE,
    CMD_INT_ENABLE,
    CMD_WRITE_BUFFER,
    CMD_READ_BUFFER,
};

static struct gf_tty_iface {
    tty* tty;
    event data_ready_evnt;
    struct {
        char buffer[512];
        uintptr_t buffer_phys;
        size_t out_ptr;
        size_t in_ptr;
    } ring_buffer;
    size_t bytes_count; // total amount of bytes ever read from this tty
    void(*data_ready)(void* tty, const void* buf, size_t nBytesReady);
    thread* data_ready_thread;
} tty_iface_obj = {
    .data_ready_evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION)
};

static void set_data_ready_cb(void* tty, void(*cb)(void* tty, const void* buf, size_t nBytesReady))
{
    struct tty* t = tty;
    struct gf_tty_iface* iface = t->interface.userdata;
    iface->data_ready = cb;
}
static obos_status write(void* tty, const char* buf, size_t szBuf)
{
    OBOS_UNUSED(tty);
    struct gf_tty* dev = (void*)Arch_TTYBase;
    for (size_t i = 0; i < szBuf; i++)
        dev->put_char = buf[i];
    return OBOS_STATUS_SUCCESS;
}

tty_interface tty_iface = {
    .userdata = &tty_iface_obj,
    .set_data_ready_cb = set_data_ready_cb,
    .write = write,
    .tcdrain = nullptr,
    .size.row=30,
    .size.col=95,
};

static void poll_gf_tty(struct gf_tty_iface* iface)
{
    while (!iface->tty)
        OBOSS_SpinlockHint();
    // struct gf_tty* dev = (void*)Arch_TTYBase;
    while (1)
    {
        Core_WaitOnObject(WAITABLE_OBJECT(iface->data_ready_evnt));
        irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
        if (iface->data_ready)
        {
            iface->data_ready(iface->tty, iface->ring_buffer.buffer, iface->ring_buffer.out_ptr);
            iface->ring_buffer.out_ptr -= iface->ring_buffer.out_ptr;
        }
        Core_LowerIrql(oldIrql);
        Core_EventClear(&iface->data_ready_evnt);
    }
    Core_ExitCurrentThread();
}

void gf_irq_hnd(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && userdata && oldIrql);
    struct gf_tty* dev = (void*)Arch_TTYBase;
    Core_EventSet(&tty_iface_obj.data_ready_evnt, false);
    tty_iface_obj.bytes_count += dev->bytes_ready;
    size_t nToRead = OBOS_MIN(dev->bytes_ready, sizeof(tty_iface_obj.ring_buffer.buffer) - tty_iface_obj.ring_buffer.out_ptr);
    if (!tty_iface_obj.ring_buffer.buffer_phys)
    {
        MmS_QueryPageInfo(Mm_KernelContext.pt, (uintptr_t)tty_iface_obj.ring_buffer.buffer, nullptr, &tty_iface_obj.ring_buffer.buffer_phys);
        tty_iface_obj.ring_buffer.buffer_phys += ((uintptr_t)tty_iface_obj.ring_buffer.buffer) % OBOS_PAGE_SIZE;
    } 
    dev->data_len = nToRead;            
    dev->data_ptr_high = 0;
    dev->data_ptr = tty_iface_obj.ring_buffer.buffer_phys + tty_iface_obj.ring_buffer.out_ptr;
    dev->command = CMD_READ_BUFFER;
    tty_iface_obj.ring_buffer.out_ptr += nToRead;
    while (dev->bytes_ready)
        OBOSS_SpinlockHint();
    // printf("%d bytes read\n", nToRead);
    // printf("%d bytes ever read\n", tty_iface_obj.bytes_count);
    // dev->command = CMD_INT_DISABLE;
}

static BootDeviceBase gf_tty_info;

void tty_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    OBOS_UNUSED(userdata);
    Arch_PICRegisterIRQ(gf_tty_info.irq, to->id + 0x40);
}

static irq gf_tty_irq;

void OBOSS_MakeTTY()
{
    gf_tty_info = *(BootDeviceBase*)(Arch_GetBootInfo(BootInfoType_GoldfishTtyBase)+1);

    tty_iface_obj.data_ready_thread = CoreH_ThreadAllocate(nullptr);
    thread_ctx ctx = {};
    void* stack = Mm_VirtualMemoryAlloc(
        &Mm_KernelContext, 
        nullptr, 0x4000,
        0, VMA_FLAGS_KERNEL_STACK, 
        nullptr,nullptr);
    CoreS_SetupThreadContext(&ctx, (uintptr_t)poll_gf_tty, (uintptr_t)&tty_iface_obj, false, stack, 0x4000);
    CoreH_ThreadInitialize(tty_iface_obj.data_ready_thread, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, tty_iface_obj.data_ready_thread);
    CoreH_ThreadReady(tty_iface_obj.data_ready_thread);

    gf_tty_info.irq = 31;

    gf_tty_irq.handler = gf_irq_hnd;
    gf_tty_irq.moveCallback = tty_irq_move_callback;
    gf_tty_irq.handlerUserdata = nullptr;
    Core_IrqObjectInitializeIRQL(&gf_tty_irq, IRQL_DISPATCH, false, true);

    Arch_PICMaskIRQ(gf_tty_info.irq, true);
    Arch_PICRegisterIRQ(gf_tty_info.irq, gf_tty_irq.vector->id + 0x40);

    struct gf_tty* dev = (void*)Arch_TTYBase;
    dev->command = CMD_INT_ENABLE;

    dirent* ent = nullptr;
    Vfs_RegisterTTY(&tty_iface, &ent, false);
    tty_iface_obj.tty = ent->vnode->data;

    Core_SetProcessGroup(OBOS_KernelProcess, 0);
    OBOS_KernelProcess->pgrp->controlling_tty = ent->vnode->data;
    ((struct tty*)ent->vnode->data)->fg_job = OBOS_KernelProcess->pgrp;

    Arch_PICMaskIRQ(gf_tty_info.irq, false);
}