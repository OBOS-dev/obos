/*
 * drivers/generic/usb-hid/main.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/usb.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>
#include <scheduler/process.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <irq/timer.h>

#include <vfs/irp.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/keycode.h>
#include <vfs/mouse.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>

#include "scancodes.h"

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_WEAK obos_status submit_irp(void*);
OBOS_WEAK obos_status finalize_irp(void*);
OBOS_WEAK obos_status ioctl(dev_desc what, uint32_t request, void* argp);
OBOS_WEAK obos_status ioctl_argp_size(uint32_t request, size_t* res);
OBOS_WEAK obos_status reference_device(dev_desc* desc);
OBOS_WEAK obos_status unreference_device(dev_desc desc);
void driver_cleanup_callback() {}

driver_id* this_driver;

OBOS_WEAK void on_wake();
OBOS_WEAK void on_suspend();

#define KEY_PRESSED(bitfield, key) (bitfield[key/8] |= BIT(key%8))
#define KEY_IS_PRESSED(bitfield, key) (bitfield[key/8] & BIT(key%8))
#define BITFIELD_DIFF(bitfield1, bitfield2, bitfield_out) \
do {\
    for (int i = 0; i < (104/8); i++)\
        bitfield_out[i] = ~bitfield1[i] & bitfield2[i];\
} while(0);

typedef struct hid_dev {
    shared_ptr ptr;
    usb_dev_desc* desc;
    event data_event;
    event worker_die_event;
    thread* worker;
    struct {
        void* buff;
        size_t len;
        size_t ptr;
    } ringbuffer;
    size_t blkSize;
    vnode* vn;
    dirent* ent;

    uint8_t pressed_keys[104/8];
    uint8_t active_modifiers[8/8];
    bool superkey : 1;

    usb_endpoint* in_endpoint;
    LIST_NODE(device_list, struct hid_dev) node;
} hid_dev;
typedef struct hid_handle {
    size_t in_ptr;
    hid_dev* dev;
} hid_handle;
typedef LIST_HEAD(device_list, hid_dev) device_list;
LIST_GENERATE_STATIC(device_list, hid_dev, node);

device_list g_hid_devices;

static uint8_t dev_idx = 0;

static obos_status initialize_device(hid_dev* dev)
{
    for (usb_endpoint* curr = dev->desc->endpoints.head; curr; curr = curr->node.next)
    {
        if (curr->direction == true)
        {
            dev->in_endpoint = curr;
            break;
        }
    }
    if (!dev->in_endpoint)
        return OBOS_STATUS_NOT_FOUND;

    usb_irp_payload set_protocol = {};
    set_protocol.endpoint = 0;
    set_protocol.trb_type = USB_TRB_CONTROL;
    set_protocol.payload.setup.nRegions = 0;
    set_protocol.payload.setup.regions = nullptr;
    set_protocol.payload.setup.bmRequestType = 0x21;
    set_protocol.payload.setup.bRequest = 0xb;
    set_protocol.payload.setup.wLength = 0;
    set_protocol.payload.setup.wIndex = 0;
    set_protocol.payload.setup.wValue = 0 /* boot protocol */;

    return Drv_USBSynchronousOperation(dev->desc, &set_protocol, false);
}

struct boot_mouse_packet {
    uint8_t flags;
    int8_t x;
    int8_t y;
} OBOS_PACK;

static void push_bytes(hid_dev* dev, const void* buff, size_t count)
{
    char* into = dev->ringbuffer.buff;
    into += (dev->ringbuffer.ptr % dev->ringbuffer.len);
    memcpy(into, buff, count);
    dev->ringbuffer.ptr += count;
    Core_EventSet(&dev->data_event, false);
}

static void hid_worker_thread(hid_dev* dev)
{
    uint8_t interval = dev->in_endpoint->descriptor.bInterval;
    // timer* tm = nullptr;
    // event evnt = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    // CoreH_MakeTimerEvent(&tm, (uint32_t)interval*1000, &evnt, true);

    uint8_t report_len = 0;
    uint8_t* report = nullptr;

    if (dev->blkSize == sizeof(keycode)) report_len = 8;
    else if (dev->blkSize == sizeof(mouse_packet)) report_len = 3;

    report = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, report_len, nullptr);

    usb_irp_payload payload = {};
    payload.trb_type = USB_TRB_NORMAL;
    payload.endpoint = dev->in_endpoint->endpoint_number;
    obos_status status = DrvH_ScatterGather(&Mm_KernelContext, report, report_len, &payload.payload.normal.regions, &payload.payload.normal.nRegions, 63, false);
    if (obos_is_error(status))
        goto exit;

    timer_tick ival = CoreH_TimeFrameToTick(interval*1000);
    int errc = 0;
    while (1)
    {
        // struct waitable_header* signaled = nullptr;
        // struct waitable_header* wobjs[2] = {
        //     WAITABLE_OBJECT(dev->worker_die_event),
        //     WAITABLE_OBJECT(evnt)
        // };

        // status = Core_WaitOnObjects(2, wobjs, &signaled);
        // if (obos_is_error(status))
        //     break;
        // if (signaled == wobjs[0])
        //     break;
        // Core_EventClear(&evnt);

        if (Core_EventGetState(&dev->worker_die_event))
            break;

        timer_tick deadline = CoreS_GetTimerTick() + ival;
#if __x86_64__
        while (CoreS_GetTimerTick() < deadline)
            asm volatile("hlt");
#else
#pragma GCC warning "Implement this for the target architecture"
        while (CoreS_GetTimerTick() < deadline)
            OBOSS_SpinlockHint()
#endif

        status = Drv_USBSynchronousOperation(dev->desc, &payload, true);
        if (obos_is_error(status))
        {
            if (errc++ < 10)
                continue;
            else
                break;
        }
        errc = 0;

        switch (report_len) {
            case 8:
            {
                // TODO(oberrow): Key repeating?

                enum modifiers obos_mods = 0;
                if ((report[0] & LEFT_CTRL) || (report[0] & RIGHT_CTRL))
                    obos_mods |= CTRL;
                if ((report[0] & LEFT_SHIFT) || (report[0] & RIGHT_SHIFT))
                    obos_mods |= SHIFT;
                if ((report[0] & LEFT_ALT) || (report[0] & RIGHT_ALT))
                    obos_mods |= ALT;
                if ((report[0] & LEFT_GUI) || (report[0] & RIGHT_GUI))
                    obos_mods |= SUPER_KEY;

                uint8_t currently_pressed_keys_bmp[104/8];
                uint8_t released_keys_bmp[104/8];
                for (int i = 2; i < 8; i++)
                {
                    uint8_t code = report[i];
                    if (code > 104 || code < 4)
                        continue;
                    KEY_PRESSED(currently_pressed_keys_bmp, code);
                }
                BITFIELD_DIFF(currently_pressed_keys_bmp, dev->pressed_keys, released_keys_bmp);
                keycode *output = nullptr;
                size_t nOutputs = 0;
                for (int i = 0, bit = 0; i < 104/8; i++, bit++)
                {
                    for (int j = 0; j < 8; j++)
                    {
                        if (released_keys_bmp[i] & BIT(j))
                        {
                            scancode sc = s_scancode_keycode_table_boot[bit];
                            enum modifiers extra_mods = IS_NUMPAD(bit)|KEY_RELEASED;
                            size_t old_size = nOutputs * sizeof(output[0]);
                            size_t new_size = (1+nOutputs) * sizeof(output[0]);
                            output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                            output[nOutputs++] = KEYCODE(sc, obos_mods|extra_mods);
                        }
                    }
                }
                for (int i = 2; i < 8; i++)
                {
                    uint8_t code = report[i];
                    if (code > 104 || code < 4)
                        continue;
                    scancode sc = s_scancode_keycode_table_boot[code];
                    enum modifiers extra_mods = IS_NUMPAD(code);
                    size_t old_size = nOutputs * sizeof(output[0]);
                    size_t new_size = (1+nOutputs) * sizeof(output[0]);
                    output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                    output[nOutputs++] = KEYCODE(sc, obos_mods|extra_mods);
                }
                if (obos_mods & CTRL)
                {
                    scancode sc = SCANCODE_CTRL;
                    size_t old_size = nOutputs * sizeof(output[0]);
                    size_t new_size = (1+nOutputs) * sizeof(output[0]);
                    output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                    output[nOutputs++] = KEYCODE(sc, obos_mods);
                }
                if (obos_mods & ALT)
                {
                    scancode sc = SCANCODE_ALT;
                    size_t old_size = nOutputs * sizeof(output[0]);
                    size_t new_size = (1+nOutputs) * sizeof(output[0]);
                    output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                    output[nOutputs++] = KEYCODE(sc, obos_mods);
                }
                if (obos_mods & SHIFT)
                {
                    scancode sc = SCANCODE_SHIFT;
                    size_t old_size = nOutputs * sizeof(output[0]);
                    size_t new_size = (1+nOutputs) * sizeof(output[0]);
                    output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                    output[nOutputs++] = KEYCODE(sc, obos_mods);
                }
                bool superkey = (report[0] & LEFT_GUI) || (report[0] & RIGHT_GUI);
                if (superkey != dev->superkey)
                {
                    scancode sc = SCANCODE_SUPER_KEY;
                    size_t old_size = nOutputs * sizeof(output[0]);
                    size_t new_size = (1+nOutputs) * sizeof(output[0]);
                    output = Reallocate(OBOS_KernelAllocator, output, new_size, old_size, nullptr);
                    output[nOutputs++] = KEYCODE(sc, obos_mods|(superkey ? 0 : KEY_RELEASED));
                }
                push_bytes(dev, output, nOutputs*sizeof(output[0]));
                Free(OBOS_KernelAllocator, output, nOutputs*sizeof(output[0]));
                break;
            }
            case 3:
            {
                // Mouse packet.
                struct boot_mouse_packet* pckt = (void*)report;
                mouse_packet output = {};
                output.lb = pckt->flags & BIT(0);
                output.rb = pckt->flags & BIT(1);
                output.mb = pckt->flags & BIT(2);
                output.b4 = 0;
                output.b5 = 0;
                output.x = pckt->x;
                output.y = -pckt->y;
                output.z = 0;
                push_bytes(dev, &output, sizeof(output));
                break;
            }
        }
        Core_Yield();
    }

    exit:
    if (payload.payload.normal.regions)
        DrvH_FreeScatterGatherList(&Mm_KernelContext, report, report_len, payload.payload.normal.regions, payload.payload.normal.nRegions);
    Free(OBOS_NonPagedPoolAllocator, report, report_len);
    OBOS_SharedPtrUnref(&dev->ptr);
    Core_ExitCurrentThread();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wframe-address"
static void hid_dev_onref(struct shared_ptr* ptr)
{
    hid_dev* dev = ptr->obj;
    OBOS_SharedPtrRef(&dev->desc->ptr);
}
static void hid_dev_ondeRef(struct shared_ptr* ptr)
{
    hid_dev* dev = ptr->obj;
    OBOS_SharedPtrUnref(&dev->desc->ptr);
}
#pragma GCC diagnostic ignored "-Wframe-address"

obos_status on_usb_attach(usb_dev_desc* desc)
{
    if (desc->info.hid.subclass == 0 /* report protocol only device */)
        return OBOS_STATUS_UNIMPLEMENTED;
    if (desc->info.hid.protocol == 0 || desc->info.hid.protocol > 2)
        return OBOS_STATUS_UNIMPLEMENTED;

    obos_status status = Drv_USBDriverAttachedToPort(desc, this_driver);
    if (obos_is_error(status))
        return status;

    hid_dev* dev = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*dev), nullptr);
    OBOS_SharedPtrConstruct(&dev->ptr, dev);
    dev->ptr.onDeref = hid_dev_ondeRef;
    dev->ptr.onRef = hid_dev_onref;
    dev->ptr.free = OBOS_SharedPtrDefaultFree;
    dev->ptr.freeUdata = OBOS_KernelAllocator;
    // OBOS_SharedPtrRef(&desc->ptr);
    // already referenced for us by Drv_PnpUSBDeviceAttached
    dev->desc = desc;

    dev->worker_die_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);

    dev->blkSize = desc->info.hid.protocol == 2 ? sizeof(mouse_packet) : sizeof(keycode);
    
    dev->data_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    
    OBOS_SharedPtrRef(&dev->ptr);
    LIST_APPEND(device_list, &g_hid_devices, dev);
    
    status = initialize_device(dev);
    if (obos_is_error(status))
    {
        OBOS_SharedPtrUnref(&dev->ptr);
        return status;
    }
    
    OBOS_SharedPtrRef(&dev->ptr);
    desc->dev_ptr = dev;

    dev_desc ddesc = (dev_desc)dev;
    reference_device(&ddesc);

    char id = 'u';
    switch (dev->blkSize) {
        case sizeof(keycode): id = 'k'; break;
        case sizeof(mouse_packet): id = 'm'; break;
        default: OBOS_UNREACHABLE;
    }

    char* name = nullptr;
    size_t name_len = snprintf(name, 0, "hid%c%d", id, dev_idx++);

    name = Allocate(OBOS_KernelAllocator, name_len+1, nullptr);
    snprintf(name, name_len+1, "hid%c%d", id, dev_idx - 1);

    dev->vn = Drv_AllocateVNode(this_driver, ddesc, 0, nullptr, VNODE_TYPE_CHR);
    dev->vn->flags |= VFLAGS_UNREFERENCE_ON_DELETE;
    dev->vn->blkSize = dev->blkSize;
    dev->ent = Drv_RegisterVNode(dev->vn, name);

    Free(OBOS_KernelAllocator, name, name_len+1);

    dev->ringbuffer.len = OBOS_PAGE_SIZE*2;
    dev->ringbuffer.ptr = 0;
    dev->ringbuffer.buff = Allocate(OBOS_KernelAllocator, dev->ringbuffer.len, nullptr);

    dev->worker = CoreH_ThreadAllocate(nullptr);
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, 0x4000, 0, VMA_FLAGS_KERNEL_STACK, nullptr, nullptr);
    thread_ctx ctx = {};
    CoreS_SetupThreadContext(&ctx, (uintptr_t)hid_worker_thread, (uintptr_t)dev, false, stack, 0x4000);
    CoreH_ThreadInitialize(dev->worker, THREAD_PRIORITY_NORMAL, Core_DefaultThreadAffinity, &ctx);
    Core_ProcessAppendThread(OBOS_KernelProcess, dev->worker);
    OBOS_SharedPtrRef(&dev->ptr);
    CoreH_ThreadReady(dev->worker);
    
    OBOS_Log("usb-hid: device bound to driver\n");

    return OBOS_STATUS_SUCCESS;
}

obos_status on_usb_detach(usb_dev_desc* desc)
{
    OBOS_Debug("usb-hid: device removed\n");
    
    hid_dev* dev = desc->dev_ptr;
    
    //CoreH_AbortWaitingThreads(WAITABLE_OBJECT(dev->data_event));
    Core_EventSet(&dev->worker_die_event, false);
    
    LIST_REMOVE(device_list, &g_hid_devices, dev);
    OBOS_SharedPtrUnref(&dev->ptr);
    
    OBOS_SharedPtrUnref(&desc->ptr);
    
    desc->dev_ptr = nullptr;
    OBOS_SharedPtrUnref(&dev->ptr);
    
    return OBOS_STATUS_SUCCESS;
}

obos_status reference_device(dev_desc* desc)
{
    hid_handle* hnd = (void*)*desc;
    if (!hnd)
        return OBOS_STATUS_INVALID_ARGUMENT;
    hid_dev* dev = hnd->dev;
    OBOS_SharedPtrRef(&dev->ptr);
    hid_handle* new_hnd = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*hnd), nullptr);
    new_hnd->dev = dev;
    new_hnd->in_ptr = dev->ringbuffer.ptr;
    *desc = (dev_desc)new_hnd;
    return OBOS_STATUS_SUCCESS;
}

obos_status unreference_device(dev_desc desc)
{
    hid_handle* hnd = (void*)desc;
    if (!hnd)
        return OBOS_STATUS_INVALID_ARGUMENT;
    OBOS_SharedPtrUnref(&hnd->dev->ptr);
    Free(OBOS_KernelAllocator, hnd, sizeof(*hnd));
    return OBOS_STATUS_SUCCESS;
}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|
             DRIVER_HEADER_FLAGS_DETECT_VIA_USB|
             DRIVER_HEADER_HAS_VERSION_FIELD|
             DRIVER_HEADER_FLAGS_USB_DO_NOT_CHECK_SUBCLASS,
    .usbHid = {
        .class = 0x03,
        .subclass = 0x00,
        .protocol = 0x00,
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .ioctl_argp_size = ioctl_argp_size,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_wake = on_wake,
        .on_suspend = on_suspend,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,
        .on_usb_attach = on_usb_attach,
        .on_usb_detach = on_usb_detach,
        .reference_device = reference_device,
        .unreference_device = unreference_device,
    },
    .driverName = "USB HID Driver",
    .version=2,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}

obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    hid_handle* hnd = (void*)desc;
    *blkSize = hnd->dev->blkSize;
    return OBOS_STATUS_SUCCESS;
}

obos_status get_max_blk_count(dev_desc desc, size_t* count)
{ OBOS_UNUSED(desc && count); return OBOS_STATUS_INVALID_OPERATION; }

obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkRead);
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkWritten);
    return OBOS_STATUS_INVALID_OPERATION;
}

static void irp_on_event_set(irp* req)
{
    hid_handle* hnd = (void*)req->desc;
    size_t available = hnd->dev->ringbuffer.ptr - hnd->in_ptr;
    if (available < (req->blkCount - req->nBlkRead))
    {
        if (hnd->dev->desc->attached)
            req->status = OBOS_STATUS_IRP_RETRY;
        else
            req->status = OBOS_STATUS_INTERNAL_ERROR;
        return;
    }
    if (req->dryOp)
    {
        req->status = OBOS_STATUS_SUCCESS;
        return;    
    }
    char* into = (char*)req->buff + req->nBlkRead * hnd->dev->blkSize;
    char* from = (char*)hnd->dev->ringbuffer.buff + (hnd->in_ptr % hnd->dev->ringbuffer.len);

    size_t nToRead = OBOS_MIN(available, req->blkCount * hnd->dev->blkSize);
    memcpy(into, from, nToRead);
    req->nBlkRead += nToRead / hnd->dev->blkSize;

    hnd->in_ptr += nToRead;

    req->status = OBOS_STATUS_SUCCESS;
    
    available = hnd->dev->ringbuffer.ptr - hnd->in_ptr;
    if (!available)
        Core_EventClear(req->evnt);
}

obos_status submit_irp(void* reqp)
{
    irp* req = reqp;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (req->op == IRP_WRITE)
    {
        req->status = OBOS_STATUS_INVALID_OPERATION;
        return OBOS_STATUS_SUCCESS;
    }

    hid_handle* hnd = (void*)req->desc;
    if (!hnd)
    {
        req->status = OBOS_STATUS_INVALID_ARGUMENT;
        return OBOS_STATUS_SUCCESS;
    }
    hid_dev* dev = hnd->dev;
    OBOS_SharedPtrRef(&dev->ptr);
    if (!dev->desc->attached)
    {
        // Not a memory leak! see finalize_irp
        req->status = OBOS_STATUS_INTERNAL_ERROR;
        return OBOS_STATUS_SUCCESS;
    }

    req->status = OBOS_STATUS_SUCCESS;
    req->evnt = &dev->data_event;
    req->detach_event = &dev->desc->on_detach;
    req->on_event_set = irp_on_event_set;

    return 0;
}
obos_status finalize_irp(void* reqp)
{
    irp* req = reqp;
    OBOS_ENSURE(req->drvData == 0);
    hid_handle* hnd = (void*)req->desc;
    req->drvData = (void*)1;
    OBOS_SharedPtrUnref(&hnd->dev->ptr);
    return OBOS_STATUS_SUCCESS;
}

obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    if (!argp || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    hid_handle* handle = (void*)what;
    hid_dev* dev = handle->dev;
    obos_status st = OBOS_STATUS_SUCCESS;
    switch (request)
    {
        case 1:
            // "A temporary solution"
            //     - me, probably, when i wrote the ps2 code
            (*(size_t*)argp) = (dev->ringbuffer.ptr - handle->in_ptr) / dev->blkSize;
            break;
        default:
            st = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return st;
}

obos_status ioctl_argp_size(uint32_t request, size_t* res)
{
    obos_status st = OBOS_STATUS_SUCCESS;
    switch (request)
    {
        case 1:
            *res = sizeof(size_t);
            break;
        default:
            st = OBOS_STATUS_INVALID_IOCTL;
            break;
    }
    return st;
}