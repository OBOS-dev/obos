/*
 * oboskrnl/driver_interface/usb.h
 *
 * Copyright (c) 2026 Omar Berrow
 */

#pragma once

#include <int.h>
#include <struct_packing.h>

#include <mm/sglist.h>

#include <locks/mutex.h>
#include <locks/event.h>

#include <utils/list.h>
#include <utils/shared_ptr.h>

typedef union {
    struct {
        uint8_t class;
        uint8_t subclass;
        uint8_t protocol;
    };
    uint32_t hid;
} usb_hid;

// TODO: Will this translate easily to EHCI+UHCI?
// (does this *need* to translate easily to EHCI+UHCI)

typedef enum usb_trb_type {
    USB_TRB_NORMAL,
    // only legal for endpoint zero (control)
    USB_TRB_SETUP,
    USB_TRB_ISOCH,
    USB_TRB_NOP,
    // NOTE: Should be done in both directions
    // NOTE: Is always invalid for the control endpoint.
    USB_TRB_CONFIGURE_ENDPOINT,
} usb_trb_type;

// TRB Direction is defined by irp.op

typedef struct usb_irp_payload {
    usb_trb_type trb_type;
    uint8_t endpoint;
    union {
        struct {
            struct physical_region* regions;
            size_t nRegions;
        } normal;
        struct {
            struct physical_region* regions;
            size_t nRegions;
        } isoch;
        struct {
            struct {
                uint8_t bmRequestType;
                uint8_t bRequest;
                uint16_t wValue;
                uint16_t wIndex;
                uint16_t wLength;
            } OBOS_PACK;
            struct physical_region* regions;
            size_t nRegions;
            // TODO: Return status in some way from the status stage TRB (xhci-only)? 
        } setup;
    } payload;
} usb_irp_payload;

typedef enum usb_device_speed {
    USB_DEVICE_LOW_SPEED, // 1.5 Mb/s
    USB_DEVICE_FULL_SPEED, // 12 Mb/s
    USB_DEVICE_HIGH_SPEED, // 480 Mb/s
    USB_DEVICE_SUPER_SPEED_GEN1_X1, // 5 Gb/s
    USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X1, // 10 Gb/s
    USB_DEVICE_SUPER_SPEED_PLUS_GEN1_X2, // 5 Gb/s
    USB_DEVICE_SUPER_SPEED_PLUS_GEN2_X2, // 10 Gb/s
} usb_device_speed;
OBOS_EXPORT const char* Drv_USBDeviceSpeedAsString(usb_device_speed val);

typedef struct usb_device_info {
    usb_hid hid;

    uint32_t address;
    uint8_t slot;

    uint8_t speed;
} usb_device_info;

typedef struct usb_dev_desc {
    shared_ptr ptr;
    
    struct usb_controller* controller;
    
    usb_device_info info;

    bool attached : 1;

    LIST_NODE(usb_devices, struct usb_dev_desc) node;
} usb_dev_desc;
typedef LIST_HEAD(usb_devices, struct usb_dev_desc) usb_devices;
LIST_PROTOTYPE(usb_devices, struct usb_dev_desc, node);

typedef struct usb_controller {
    void* handle;
    struct driver_header* hdr;

    usb_devices ports;
    mutex ports_lock;
    struct {
        event on_attach;
        event on_detach;
    } port_events;
    
    LIST_NODE(usb_controller_list, struct usb_controller) node;
} usb_controller;
typedef LIST_HEAD(usb_controller_list, struct usb_controller) usb_controller_list;
LIST_PROTOTYPE(usb_controller_list, struct usb_controller, node);

extern usb_controller_list Drv_USBControllers;
extern mutex Drv_USBControllersLock;

OBOS_EXPORT obos_status Drv_USBControllerRegister(void* handle, struct driver_header* header, usb_controller** out);

OBOS_EXPORT obos_status Drv_USBPortAttached(usb_controller* ctlr, const usb_device_info* info, usb_dev_desc** desc);
OBOS_EXPORT obos_status Drv_USBPortDetached(usb_controller* ctlr, usb_dev_desc* desc);
