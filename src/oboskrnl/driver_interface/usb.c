/*
 * oboskrnl/driver_interface/usb.c
 *
 * Copyright (c) 2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <struct_packing.h>

#include <mm/sglist.h>
#include <mm/pmm.h>

#include <driver_interface/usb.h>
#include <driver_interface/header.h>

#include <allocators/base.h>

#include <vfs/irp.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>

LIST_GENERATE(usb_devices, struct usb_dev_desc, node);
LIST_GENERATE(usb_controller_list, struct usb_controller, node);

usb_controller_list Drv_USBControllers;
mutex Drv_USBControllersLock = MUTEX_INITIALIZE();

obos_status Drv_USBControllerRegister(void* handle, struct driver_header* header, usb_controller** out)
{
    if (!header || !out)
        return OBOS_STATUS_INVALID_ARGUMENT;

    usb_controller* ctlr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*ctlr), nullptr);
    ctlr->handle = handle;
    ctlr->hdr = header;
    ctlr->ports_lock = MUTEX_INITIALIZE();
    ctlr->port_events.on_attach = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    ctlr->port_events.on_detach = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    
    Core_MutexAcquire(&Drv_USBControllersLock);
    LIST_APPEND(usb_controller_list, &Drv_USBControllers, ctlr);
    Core_MutexRelease(&Drv_USBControllersLock);

    *out = ctlr;
    return OBOS_STATUS_SUCCESS;
}

const char* Drv_USBDeviceSpeedAsString(usb_device_speed val)
{
    switch (val) {
        case USB_DEVICE_LOW_SPEED: return "low-speed";
        case USB_DEVICE_FULL_SPEED: return "full-speed";
        case USB_DEVICE_HIGH_SPEED: return "high-speed";
        case USB_DEVICE_SUPER_SPEED_GEN1_X1: return "superspeed gen1 x1";
        case USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X1: return "superspeed+ gen2 x1";
        case USB_DEVICE_SUPER_SPEED_PLUS_GEN1_X2: return "superspeed+ gen1 x1";
        case USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X2: return "superspeed+ gen2 x2";
        default: return "unknown speed";
    }
}

obos_status Drv_USBPortAttached(usb_controller* ctlr, const usb_device_info* info, usb_dev_desc** odesc)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !info)
        return OBOS_STATUS_INVALID_ARGUMENT;

    usb_dev_desc* desc = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*desc), nullptr);
    OBOS_SharedPtrConstruct(&desc->ptr, desc);
    desc->ptr.free = OBOS_SharedPtrDefaultFree;
    desc->ptr.freeUdata = OBOS_KernelAllocator;
    OBOS_SharedPtrRef(&desc->ptr);

    desc->attached = true;
    desc->controller = ctlr;
    desc->info = *info;

    Core_MutexAcquire(&ctlr->ports_lock);
    LIST_APPEND(usb_devices, &ctlr->ports, desc);
    Core_MutexRelease(&ctlr->ports_lock);

    Core_EventSet(&ctlr->port_events.on_attach, false);

    OBOS_Debug("usb: %s port attached on address 0x%x\n", Drv_USBDeviceSpeedAsString(info->speed), info->address);

    if (odesc) *odesc = desc;
    
    return OBOS_STATUS_SUCCESS;
}

OBOS_EXPORT obos_status Drv_USBPortPostAttached(usb_controller* ctlr, usb_dev_desc* desc)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_USBPortDetached(usb_controller* ctlr, usb_dev_desc* desc)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ENSURE(ctlr == desc->controller);

    Core_MutexAcquire(&ctlr->ports_lock);
    LIST_REMOVE(usb_devices, &ctlr->ports, desc);
    Core_MutexRelease(&ctlr->ports_lock);
    
    desc->attached = false;
    OBOS_SharedPtrUnref(&desc->ptr);

    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_USBSubmitIRP(usb_dev_desc* desc, void* req_p)
{
    if (!desc || !desc->controller || !desc->controller->hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* req = req_p;
    req->desc = (dev_desc)desc;
    return desc->controller->hdr->ftable.submit_irp(req_p);
}
OBOS_EXPORT obos_status Drv_USBIRPWait(usb_dev_desc* desc, void* req)
{
    OBOS_ENSURE(Core_GetIrql() <= IRQL_DISPATCH);
    irp* request = req;
    if (!request)
        return OBOS_STATUS_INVALID_ARGUMENT;
    while (request->evnt)
    {
        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(*request->evnt));
        if (obos_is_error(status))
            return status;
        if (request->on_event_set)
            request->on_event_set(request);
        if (request->status != OBOS_STATUS_IRP_RETRY)
            break;
    }
    driver_header* driver = desc->controller->hdr;
    if (driver->ftable.finalize_irp)
        driver->ftable.finalize_irp(request);
    return request->status;
}
