/*
 * oboskrnl/driver_interface/usb.c
 *
 * Copyright (c) 2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <struct_packing.h>

#include <mm/sglist.h>

#include <driver_interface/usb.h>
#include <driver_interface/header.h>

#include <allocators/base.h>

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

obos_status Drv_USBPortAttached(usb_controller* ctlr, const usb_device_info* info, usb_dev_desc** odesc)
{
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

    if (odesc) *odesc = desc;
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_USBPortDetached(usb_controller* ctlr, usb_dev_desc* desc)
{
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
