/*
 * drivers/generic/usb-keyboard/main.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/usb.h>
#include <driver_interface/driverId.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <vfs/irp.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/keycode.h>

#include <allocators/base.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>

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

obos_status on_usb_attach(usb_dev_desc* desc)
{
    if (desc->info.hid.protocol != 1)
        return OBOS_STATUS_UNIMPLEMENTED;

    obos_status status = Drv_USBDriverAttachedToPort(desc, this_driver);
    if (obos_is_error(status))
        return status;

    OBOS_Debug("usb-hid: device bound to driver\n");

    hid_dev* dev = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*dev), nullptr);
    OBOS_SharedPtrConstruct(&dev->ptr, dev);
    OBOS_SharedPtrRef(&dev->ptr);
    dev->ptr.free = OBOS_SharedPtrDefaultFree;
    dev->ptr.freeUdata = OBOS_KernelAllocator;

    dev->blkSize = sizeof(keycode);
    if (desc->info.hid.protocol != 1)
        OBOS_ENSURE(dev->blkSize != sizeof(keycode));

    dev->desc = desc;
    dev->data_event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    
    LIST_APPEND(device_list, &g_hid_devices, dev);
    
    OBOS_SharedPtrRef(&dev->ptr);
    desc->dev_ptr = dev;

    dev_desc ddesc = (dev_desc)dev;
    reference_device(&ddesc);

    char* name = nullptr;
    size_t name_len = snprintf(name, 0, "usb-hid-%d", dev_idx++);

    name = Allocate(OBOS_KernelAllocator, name_len+1, nullptr);
    snprintf(name, name_len, "usb-hid-%d", dev_idx - 1);

    dev->vn = Drv_AllocateVNode(this_driver, ddesc, 0, nullptr, VNODE_TYPE_CHR);
    dev->vn->flags |= VFLAGS_UNREFERENCE_ON_DELETE;
    dev->ent = Drv_RegisterVNode(dev->vn, name);

    Free(OBOS_KernelAllocator, name, name_len);

    return OBOS_STATUS_SUCCESS;
}

obos_status on_usb_detach(usb_dev_desc* desc)
{
    OBOS_Debug("usb-hid: device removed\n");
    
    hid_dev* dev = desc->dev_ptr;
    
    CoreH_AbortWaitingThreads(WAITABLE_OBJECT(dev->data_event));
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
    Core_EventClear(req->evnt);
    size_t available = hnd->dev->ringbuffer.ptr - hnd->in_ptr;
    if (!available)
    {
        req->status = OBOS_STATUS_IRP_RETRY;
        return;
    }
    req->nBlkRead += available / hnd->dev->blkSize;
    char* into = (char*)req->buff + req->nBlkRead * hnd->dev->blkSize;
    char* from = (char*)hnd->dev->ringbuffer.buff + (hnd->in_ptr % hnd->dev->ringbuffer.len);

    // TODO: Is this sufficient to avoid buffer overflows?
    if (available > (hnd->dev->ringbuffer.len - hnd->in_ptr))
        available = (hnd->dev->ringbuffer.len - hnd->in_ptr);

    memcpy(into, from, available);

    hnd->in_ptr += available;
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
    req->status = OBOS_STATUS_SUCCESS;
    req->evnt = &dev->data_event;
    req->on_event_set = irp_on_event_set;

    return 0;
}
obos_status finalize_irp(void* reqp)
{
    irp* req = reqp;
    hid_handle* hnd = (void*)req->desc;
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
            (*(size_t*)argp) = handle->in_ptr - dev->ringbuffer.ptr;
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