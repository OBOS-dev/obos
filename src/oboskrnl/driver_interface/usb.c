/*
 * oboskrnl/driver_interface/usb.c
 *
 * Copyright (c) 2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <struct_packing.h>
#include <memmanip.h>

#include <mm/sglist.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <irq/timer.h>

#include <driver_interface/usb.h>
#include <driver_interface/header.h>
#include <driver_interface/pnp.h>

#include <allocators/base.h>

#include <vfs/irp.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>
#include <utils/string.h>

LIST_GENERATE(usb_devices, struct usb_dev_desc, node);
LIST_GENERATE(usb_controller_list, struct usb_controller, node);
LIST_GENERATE(usb_endpoint_list, struct usb_endpoint, node);

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

static void free_endpoint_list(usb_dev_desc* desc)
{
    for (usb_endpoint* ep = LIST_GET_HEAD(usb_endpoint_list, &desc->endpoints); ep; )
    {
        usb_endpoint* const next = LIST_GET_NEXT(usb_endpoint_list, &desc->endpoints, ep);
        LIST_REMOVE(usb_endpoint_list, &desc->endpoints, ep);
        Free(OBOS_KernelAllocator, ep, sizeof(*ep));
        ep = next;
    }
}

static void free_usb_port(void* udata, shared_ptr* obj)
{
    usb_dev_desc* desc = obj->obj;
    free_endpoint_list(desc);
    Free(udata, desc, obj->szObj);
}

obos_status Drv_USBPortAttached(usb_controller* ctlr, const usb_device_info* info, usb_dev_desc** odesc, usb_dev_desc* parent)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !info)
        return OBOS_STATUS_INVALID_ARGUMENT;

    usb_dev_desc* desc = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*desc), nullptr);
    OBOS_SharedPtrConstruct(&desc->ptr, desc);
    desc->ptr.free = free_usb_port;
    desc->ptr.freeUdata = OBOS_KernelAllocator;
    OBOS_SharedPtrRef(&desc->ptr);

    desc->attached = true;
    desc->on_detach = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    desc->controller = ctlr;
    desc->children_lock = MUTEX_INITIALIZE();
    desc->info = *info;

    if (!parent)
    {
        Core_MutexAcquire(&ctlr->ports_lock);
        LIST_APPEND(usb_devices, &ctlr->ports, desc);
        Core_MutexRelease(&ctlr->ports_lock);
    }
    else
    {
        Core_MutexAcquire(&parent->children_lock);
        LIST_APPEND(usb_devices, &parent->children, desc);
        Core_MutexRelease(&parent->children_lock);
    }
    desc->parent = parent;

    if (odesc) *odesc = desc;
    
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_descriptor(usb_dev_desc* desc, uint8_t type, uint8_t idx, uint8_t length, void* buff)
{
    usb_irp_payload payload = {};

    obos_status status = DrvH_ScatterGather(&Mm_KernelContext, buff, length, &payload.payload.setup.regions, &payload.payload.setup.nRegions, 61, true);
    if (obos_is_error(status))
        return status;

    payload.trb_type = USB_TRB_CONTROL;
    payload.endpoint = 0;
    payload.payload.setup.bmRequestType = 0x80;
    payload.payload.setup.bRequest = USB_GET_DESCRIPTOR;
    payload.payload.setup.wValue = ((uint16_t)type << 8) | idx;
    payload.payload.setup.wLength = length;

    status = Drv_USBSynchronousOperation(desc, &payload, true);
    
    DrvH_FreeScatterGatherList(&Mm_KernelContext, buff, length, payload.payload.setup.regions, payload.payload.setup.nRegions);
    
    return status;
}
static obos_status get_class_descriptor(usb_dev_desc* desc, uint8_t type, uint8_t idx, uint8_t length, void* buff)
{
    usb_irp_payload payload = {};

    obos_status status = DrvH_ScatterGather(&Mm_KernelContext, buff, length, &payload.payload.setup.regions, &payload.payload.setup.nRegions, 61, true);
    if (obos_is_error(status))
        return status;

    payload.trb_type = USB_TRB_CONTROL;
    payload.endpoint = 0;
    payload.payload.setup.bmRequestType = 0xa0;
    payload.payload.setup.bRequest = USB_GET_DESCRIPTOR;
    payload.payload.setup.wValue = ((uint16_t)type << 8) | idx;
    payload.payload.setup.wLength = length;

    status = Drv_USBSynchronousOperation(desc, &payload, true);
    
    DrvH_FreeScatterGatherList(&Mm_KernelContext, buff, length, payload.payload.setup.regions, payload.payload.setup.nRegions);
    
    return status;
}

struct interface {
    usb_interface_descriptor* descriptor;
    usb_endpoint_descriptor** endpoints;
    size_t endpoint_count;
};

static obos_status configure_endpoint(usb_dev_desc* desc, usb_endpoint_descriptor* endpoint, usb_hub_info* hub_info, bool dc)
{
    usb_irp_payload conf_ep = {};
    conf_ep.trb_type = USB_TRB_CONFIGURE_ENDPOINT;
    conf_ep.payload.configure_endpoint.deconfigure = dc;
    if (hub_info)
    {
        conf_ep.payload.configure_endpoint.hub_info = *hub_info;
        conf_ep.payload.configure_endpoint.is_hub = true;
    }
    if (!dc)
    {
        conf_ep.payload.configure_endpoint.endpoint_type = endpoint->bmAttributes & 0b11;
        // TODO(oberrow): Maximum burst size
        conf_ep.payload.configure_endpoint.max_burst_size = 0;
        conf_ep.payload.configure_endpoint.max_packet_size = endpoint->wMaxPacketSize & 2047;
    }
    conf_ep.endpoint = endpoint->bEndpointAddress & 0xf;
    bool dir = (endpoint->bEndpointAddress & BIT(7));

    return Drv_USBSynchronousOperation(desc, &conf_ep, dir);
}

static obos_status configure_interface_eps(usb_dev_desc* desc, struct interface* iface)
{
    for (size_t i = 0; i < iface->endpoint_count; i++)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        if (desc->info.hid.class != 9)
            configure_endpoint(desc, iface->endpoints[i], nullptr, false);
        if (obos_is_error(status))
        {
            for (size_t j = 0; j < i; j++)
                configure_endpoint(desc, iface->endpoints[j], nullptr, true);
            free_endpoint_list(desc);
            return status;
        }
        
        usb_endpoint* ep = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(*ep), nullptr);
        ep->endpoint_number = iface->endpoints[i]->bEndpointAddress & 0xf;
        ep->dev = desc;
        ep->type = iface->endpoints[i]->bmAttributes & 0b11;
        ep->direction = (iface->endpoints[i]->bEndpointAddress & BIT(7));
        ep->descriptor = *iface->endpoints[i];
        LIST_APPEND(usb_endpoint_list, &desc->endpoints, ep);

        continue;
    }
    if (!desc->info.hid.class)
    {
        desc->info.hid.class = iface->descriptor->bInterfaceClass;
        desc->info.hid.subclass = iface->descriptor->bInterfaceSubclass;
        desc->info.hid.protocol = iface->descriptor->bInterfaceProtocol;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status try_configuration(usb_dev_desc* ddesc, usb_configuration_descriptor* conf_desc, void* top)
{
    struct interface* interfaces = nullptr;
    size_t nInterfaces = 0;
    struct interface* current_interface = nullptr;

    usb_descriptor_header* curr = usb_next_descriptor(conf_desc);
    while (((uintptr_t)curr) < ((uintptr_t)top))
    {
        switch (curr->bDescriptorType) {
            case USB_DESCRIPTOR_TYPE_INTERFACE:
            {
                interfaces = Reallocate(OBOS_KernelAllocator, interfaces, (nInterfaces+1)*sizeof(*interfaces), nInterfaces*sizeof(*interfaces), nullptr);
                current_interface = &interfaces[nInterfaces++];
                current_interface->descriptor = (void*)curr;
                current_interface->endpoint_count = 0;
                current_interface->endpoints = nullptr;
                break;
            }
            case USB_DESCRIPTOR_TYPE_ENDPOINT:
            {
                if (!current_interface)
                    break;
                size_t new_size = (current_interface->endpoint_count+1) * sizeof(usb_endpoint_descriptor*);
                size_t old_size = current_interface->endpoint_count * sizeof(usb_endpoint_descriptor*);
                current_interface->endpoints = Reallocate(OBOS_KernelAllocator, current_interface->endpoints, new_size, old_size, nullptr);
                usb_endpoint_descriptor** ep = &current_interface->endpoints[current_interface->endpoint_count++];
                (*ep) = (void*)curr;
                break;
            }
            default: break;
        }
        
        curr = usb_next_descriptor(curr);
    }

    obos_status status = OBOS_STATUS_SUCCESS;

    // We have a map of the interface and endpoint descriptors,
    // now try to initialize the endpoints.

    usb_irp_payload set_configuration = {};
    set_configuration.trb_type = USB_TRB_CONTROL;
    set_configuration.payload.setup.nRegions = 0;
    set_configuration.payload.setup.bmRequestType = 0x00;
    set_configuration.payload.setup.bRequest = USB_SET_CONFIGURATION;
    set_configuration.payload.setup.wValue = conf_desc->bConfigurationValue;
    set_configuration.payload.setup.wIndex = 0;
    set_configuration.payload.setup.wLength = 0;

    ddesc->configuration.configuration_id = conf_desc->bConfigurationValue;

    status = Drv_USBSynchronousOperation(ddesc, &set_configuration, false);
    if (obos_is_error(status))
        return status;
    
    for (size_t i = 0; i < nInterfaces; i++)
    {
        status = configure_interface_eps(ddesc, &interfaces[i]);
        Free(OBOS_KernelAllocator, interfaces[i].endpoints, interfaces[i].endpoint_count * sizeof(interfaces[i].endpoints[0]));
        if (obos_is_success(status))
            break;
    }
    Free(OBOS_KernelAllocator, interfaces, nInterfaces*sizeof(*interfaces));

    return status;
}

OBOS_EXPORT obos_status Drv_USBPortPostAttached(usb_controller* ctlr, usb_dev_desc* desc)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ALIGN(32) usb_device_descriptor dev_desc = {};
    OBOS_ENSURE((((uintptr_t)&dev_desc) & 0xf) == 0 && "unaligned pointer");
    obos_status status = get_descriptor(desc, USB_DESCRIPTOR_TYPE_DEVICE, 0, sizeof(dev_desc), &dev_desc);
    if (obos_is_error(status))
        return status;

    desc->info.hid.class = dev_desc.bDeviceClass;
    desc->info.hid.subclass = dev_desc.bDeviceSubclass;
    desc->info.hid.protocol = dev_desc.bDeviceProtocol;

    for (uint8_t conf = 0; conf < dev_desc.bNumConfigurations; conf++)
    {
        OBOS_ALIGN(32) usb_configuration_descriptor pre_conf_desc = {};
        OBOS_ENSURE((((uintptr_t)&pre_conf_desc) & 0xf) == 0 && "unaligned pointer");
        status = get_descriptor(desc, USB_DESCRIPTOR_TYPE_CONFIGURATION, 0, sizeof(pre_conf_desc), &pre_conf_desc);
        if (obos_is_error(status))
            return status;
        
        usb_configuration_descriptor* conf_desc = Allocate(OBOS_KernelAllocator, pre_conf_desc.wTotalLength, nullptr);
        void* top = (void*)((uintptr_t)conf_desc + pre_conf_desc.wTotalLength);
        status = get_descriptor(desc, USB_DESCRIPTOR_TYPE_CONFIGURATION, 0, pre_conf_desc.wTotalLength, conf_desc);
        if (obos_is_error(status))
        {
            Free(OBOS_KernelAllocator, conf_desc, pre_conf_desc.wTotalLength);
            return status;
        }

        status = try_configuration(desc, conf_desc, top);
        Free(OBOS_KernelAllocator, conf_desc, pre_conf_desc.wTotalLength);
        if (obos_is_success(status))
        {
            desc->configuration.configuration_idx = conf;
            break;
        }
    }

    if (obos_is_success(status))
    {
        char* address_str = Drv_USBMakePhysicalLocationString(desc);
        OBOS_Debug("usb: device connected on port %s\n", address_str);
        OBOS_Debug("usb: note: hid is %02x:%02x:%02x\n", desc->info.hid.class, desc->info.hid.subclass, desc->info.hid.protocol);
        Free(OBOS_KernelAllocator, address_str, strlen(address_str)+1);
    }

    if (desc->info.hid.class == 0x9)
        return Drv_USBHubAttached(desc);
    return Drv_PnpUSBDeviceAttached(desc);
}

obos_status Drv_USBPortDetached(usb_controller* ctlr, usb_dev_desc* desc)
{
    OBOS_ENSURE(Core_GetIrql() < IRQL_DISPATCH);

    if (!ctlr || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_ENSURE(ctlr == desc->controller);

    if (!desc->parent)
    {
        Core_MutexAcquire(&ctlr->ports_lock);
        LIST_REMOVE(usb_devices, &ctlr->ports, desc);
        Core_MutexRelease(&ctlr->ports_lock);
    }
    else
    {
        Core_MutexAcquire(&desc->parent->children_lock);
        LIST_REMOVE(usb_devices, &desc->parent->children, desc);
        Core_MutexRelease(&desc->parent->children_lock);
    }

    if (desc->drv)
    {
        driver_id* drv = desc->drv;
        if (drv->header.ftable.on_usb_detach)
            drv->header.ftable.on_usb_detach(desc);
        else
            OBOS_Debug("usb: driver does not have on_usb_detach callback\n");
    }

    Core_EventSet(&desc->on_detach, false);
    
    desc->attached = false;
    OBOS_SharedPtrUnref(&desc->ptr);

    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_USBDriverAttachedToPort(usb_dev_desc* desc, void* drv_id)
{
    if (!desc) return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc->drv_ptr) return OBOS_STATUS_ALREADY_INITIALIZED;
    if (!desc->attached) return OBOS_STATUS_NOT_FOUND;
    desc->drv = drv_id;
    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_USBIRPSubmit(usb_dev_desc* desc, void* req_p)
{
    if (!desc || !desc->controller || !desc->controller->hdr)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* req = req_p;
    req->desc = (dev_desc)desc;
    req->detach_event = &desc->on_detach;
    return desc->controller->hdr->ftable.submit_irp(req_p);
}

OBOS_EXPORT obos_status Drv_USBIRPSubmit2(usb_dev_desc* desc, void** reqo, const usb_irp_payload* payload, bool dir)
{
    if (!desc || !desc->controller || !desc->controller->hdr || !reqo || !payload)
        return OBOS_STATUS_INVALID_ARGUMENT;

    irp* req = VfsH_IRPAllocate();
    if (dir)
        req->op = IRP_READ;
    else
        req->op = IRP_WRITE;
    req->blkCount = sizeof(*payload);
    req->buff = Allocate(OBOS_KernelAllocator, sizeof(*payload), nullptr);
    memcpy(req->buff, payload, sizeof(*payload));

    *reqo = req;
    return Drv_USBIRPSubmit(desc, req);
}

OBOS_EXPORT obos_status Drv_USBIRPWait(usb_dev_desc* desc, void* req)
{
    OBOS_ENSURE(Core_GetIrql() <= IRQL_DISPATCH);
    irp* request = req;
    if (!request)
        return OBOS_STATUS_INVALID_ARGUMENT;
    struct waitable_header* signaled = nullptr;
    struct waitable_header* objs[2] = {};
    while (request->evnt)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        if (!request->detach_event)
            status = Core_WaitOnObject(WAITABLE_OBJECT(*request->evnt));
        else
        {
            objs[0] = WAITABLE_OBJECT(*request->evnt);
            objs[1] = WAITABLE_OBJECT(*request->detach_event);
            status = Core_WaitOnObjects(2, objs, &signaled);
            if (signaled == objs[1])
                status = OBOS_STATUS_INTERNAL_ERROR; // io error
        }
        if (obos_is_error(status))
        {
            driver_header* driver = desc->controller->hdr;
            if (driver->ftable.finalize_irp)
                driver->ftable.finalize_irp(request);
            return status;
        }
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

OBOS_EXPORT obos_status Drv_USBSynchronousOperation(usb_dev_desc* desc, const usb_irp_payload* payload, bool dir)
{
    irp* req = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;
    
    status = Drv_USBIRPSubmit2(desc, (void*)&req, payload, dir);
    if (obos_is_error(status))
    {
        if (req)
            VfsH_IRPUnref(req);
        return status;
    }
    status = Drv_USBIRPWait(desc, req);
    VfsH_IRPUnref(req);
    return status;
}

uint32_t Drv_USBMakeRouteString(usb_dev_desc* desc)
{
    uint32_t res = 0;
    for (usb_dev_desc* cur = desc; ; cur = cur->parent)
    {
        if (!cur->parent)
            break;
        res <<= 4;
        res |= cur->info.port;
        res &= ~(0xff << 24);
        res |= cur->parent->info.port << 24;
    }
    return res;
}

char* Drv_USBMakePhysicalLocationString(usb_dev_desc* desc)
{
    usb_dev_desc* reverse_hierarchy[6] = {};
    int i = 0;
    int depth = 0;
    for (usb_dev_desc* cur = desc; cur; cur = cur->parent, depth++);
    depth--;
    for (usb_dev_desc* cur = desc; cur && i < 6; cur = cur->parent)
        reverse_hierarchy[depth - i++] = cur;

    char curr[12] = {};
    string inter = {};

    for (int i = 0; i < 6; i++)
    {
        if (!reverse_hierarchy[i])
            break;

        char end = (i == 5 || !reverse_hierarchy[i+1]) ? '\0' : '.';
        snprintf(curr, sizeof(curr), "%d%c", reverse_hierarchy[i]->info.port, end);

        OBOS_AppendStringC(&inter, curr);

        memzero(curr, sizeof(curr));
    }

    char* out = Allocate(OBOS_KernelAllocator, OBOS_GetStringSize(&inter)+1, nullptr);
    memcpy(out, OBOS_GetStringCPtr(&inter), OBOS_GetStringSize(&inter));
    out[OBOS_GetStringSize(&inter)] = 0;

    return out;
}

enum {
    PORT_CONNECTION = 0,
    PORT_ENABLE = 1,
    PORT_SUSPEND = 2,
    PORT_OVER_CURRENT = 3,
    PORT_RESET = 4,
    PORT_POWER = 8,
    PORT_LOW_SPEED = 9,
    C_PORT_CONNECTION = 16,
    C_PORT_ENABLE = 17,
    C_PORT_SUSPEND = 18,
    C_PORT_OVER_CURRENT = 19,
    C_PORT_RESET = 20,
    PORT_TEST = 21,
    PORT_INDICATOR = 22,
};

static obos_status hub_port_set_feature(usb_dev_desc* desc, uint8_t request, uint8_t port, uint8_t feature_selector)
{
    usb_irp_payload payload = {};
    payload.endpoint = 0;
    payload.trb_type = USB_TRB_CONTROL;
    payload.payload.setup.nRegions = 0;
    payload.payload.setup.regions = nullptr;
    payload.payload.setup.wLength = 0;
    payload.payload.setup.bmRequestType = 0x23;
    payload.payload.setup.bRequest = request;
    payload.payload.setup.wValue = feature_selector;
    payload.payload.setup.wIndex = port;
    
    return Drv_USBSynchronousOperation(desc, &payload, false);
}

static obos_status hub_get_port_status(usb_dev_desc* desc, uint8_t port, uint16_t *out)
{
    OBOS_ALIGNAS(32) uint16_t buf[2];

    usb_irp_payload payload = {};
    payload.endpoint = 0;
    payload.trb_type = USB_TRB_CONTROL;
    obos_status status = DrvH_ScatterGather(&Mm_KernelContext, buf, 4, &payload.payload.setup.regions, &payload.payload.setup.nRegions, 61, true);
    if (obos_is_error(status))
        return status;
    payload.payload.setup.wLength = 4;
    payload.payload.setup.bmRequestType = 0xa3;
    payload.payload.setup.bRequest = USB_GET_STATUS;
    payload.payload.setup.wValue = 0;
    payload.payload.setup.wIndex = port;

    status = Drv_USBSynchronousOperation(desc, &payload, true);
   
    memcpy(out, buf, 4);
    
    DrvH_FreeScatterGatherList(&Mm_KernelContext, buf, 4, payload.payload.setup.regions, payload.payload.setup.nRegions);
    
    return status;
}

static void busy_sleep(uint32_t ms)
{
    timer_tick deadline = CoreS_GetTimerTick() + CoreH_TimeFrameToTick(ms*1000);
    while (CoreS_GetTimerTick() < deadline)
        OBOSS_SpinlockHint();
}

obos_status Drv_USBHubAttached(usb_dev_desc* desc)
{
    OBOS_ASSERT(desc);

    desc->is_hub = true;

    OBOS_ALIGNAS(32) usb_hub_descriptor pre_hub_desc = {};
    obos_status status = get_class_descriptor(desc, USB_DESCRIPTOR_TYPE_HUB, 0, sizeof(usb_hub_descriptor), &pre_hub_desc);
    if (obos_is_error(status))
        return status;
    // size_t nBytes = (pre_hub_desc.bNbrPorts / 8) + !!(pre_hub_desc.bNbrPorts % 8) + sizeof(pre_hub_desc);
    size_t nBytes = pre_hub_desc.bLength;

    usb_hub_descriptor* hub_descriptor = Allocate(OBOS_KernelAllocator, nBytes, nullptr);
    status = get_class_descriptor(desc, USB_DESCRIPTOR_TYPE_HUB, 0, nBytes, hub_descriptor);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, hub_descriptor, nBytes);
        return status;
    }

    usb_hub_info hub_info = {};
    hub_info.port_count = hub_descriptor->bNbrPorts;
    hub_info.mtt = desc->info.hid.protocol == 2;
    hub_info.route_string = Drv_USBMakeRouteString(desc);
    hub_info.tt_think_time = (hub_descriptor->wHubCharacteristics >> 5) & 0b11;
    hub_info.parent_slot_id = desc->parent ? desc->parent->info.slot : 0;

    // TODO(oberrow): Enable the MTT interface of the hub (something to do with set interface?)
    if (hub_info.mtt)
        hub_info.mtt = false;

    usb_irp_payload configure_hubp = {};
    configure_hubp.trb_type = USB_TRB_CONFIGURE_HUB;
    configure_hubp.payload.configure_hub = hub_info;
    configure_hubp.endpoint = 0;
    status = Drv_USBSynchronousOperation(desc, &configure_hubp, false);
    if (obos_is_error(status))
    {
        Free(OBOS_KernelAllocator, hub_descriptor, nBytes);
        return status;
    }

    for (usb_endpoint* ep = desc->endpoints.head; ep; ep = ep->node.next)
        configure_endpoint(desc, &ep->descriptor, &hub_info, false);

    desc->hub.descriptor = hub_descriptor;
    desc->hub.info = hub_info;

    // Power on all ports.
    for (int port = 1; port <= hub_info.port_count; port++)
    {
        status = OBOS_STATUS_SUCCESS;

        status = hub_port_set_feature(desc, USB_SET_FEATURE, port, PORT_POWER);
        if (obos_is_error(status))
            continue;
        busy_sleep(hub_descriptor->bPowerOnGood * 2);

        status = hub_port_set_feature(desc, USB_CLEAR_FEATURE, port, C_PORT_CONNECTION);
        if (obos_is_error(status))
            continue;

        uint16_t port_status[2] = {};
        status = hub_get_port_status(desc, port, port_status);
        if (obos_is_error(status))
            continue;

        if (port_status[0] & BIT(0))
        {
            status = hub_port_set_feature(desc, USB_SET_FEATURE, port, PORT_RESET);
            if (obos_is_error(status))
                continue;

            for (size_t iters = 0; iters < 10; iters++)
            {
                status = hub_get_port_status(desc, port, port_status);
                if (obos_is_error(status))
                    break;

                if (port_status[0] & BIT(1))
                    break;

                busy_sleep(1);
            }
            if (obos_is_error(status))
                continue;

            if (~port_status[0] & BIT(1))
            {
                OBOS_Debug("usb: could not reset port on hub: timed out\n");
                continue;
            }

            usb_device_speed speed = 0;
            if (port_status[0] & BIT(9))
                speed = USB_DEVICE_LOW_SPEED;
            else if (port_status[0] & BIT(10))
                speed = USB_DEVICE_HIGH_SPEED;
            else
                speed = USB_DEVICE_FULL_SPEED;

            usb_device_info dev_info = {};
            dev_info.address = 0;
            dev_info.port = port;
            dev_info.speed = speed;
            dev_info.slot = 0;
            dev_info.usb3 = false;
            
            usb_dev_desc* new_desc = nullptr;
            status = Drv_USBPortAttached(desc->controller, &dev_info, &new_desc, desc);
            OBOS_ENSURE(obos_is_success(status));

            usb_ctlr_ioctl_slot_allocate ioctl_arg = {};
            ioctl_arg.is_hub = false;
            ioctl_arg.port_number = dev_info.port;
            ioctl_arg.route_string = Drv_USBMakeRouteString(new_desc);

            driver_header* header = desc->controller->hdr;
            status = header->ftable.ioctl((dev_desc)desc->controller->handle, IOCTL_USB_CTLR_ALLOCATE_SLOT, &ioctl_arg);
            if (obos_is_error(status))
            {
                Core_MutexAcquire(&desc->parent->children_lock);
                LIST_REMOVE(usb_devices, &desc->parent->children, desc);
                Core_MutexRelease(&desc->parent->children_lock);
                OBOS_SharedPtrUnref(&new_desc->ptr);
                continue;
            }

            new_desc->info.address = ioctl_arg.address;
            new_desc->info.slot = ioctl_arg.slot;

            Drv_USBPortPostAttached(desc->controller, new_desc);
        }
    }

    return OBOS_STATUS_SUCCESS;
}