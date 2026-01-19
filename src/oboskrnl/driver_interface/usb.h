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

enum {
    USB_GET_STATUS = 0,
    USB_CLEAR_FEATURE = 1,
    USB_SET_FEATURE = 3,
    USB_SET_ADDRESS = 5,
    USB_GET_DESCRIPTOR = 6,
    USB_SET_DESCRIPTOR = 7,
    USB_GET_CONFIGURATION = 8,
    USB_SET_CONFIGURATION = 9,
    USB_GET_INTERFACE = 10,
    USB_SET_INTERFACE = 11,
    USB_SYNCH_FRAME = 12,
    USB_SET_SEL = 48,
    USB_SET_ISOCH_DELAY = 49,
};

enum {
    USB_DESCRIPTOR_TYPE_DEVICE = 1,
    USB_DESCRIPTOR_TYPE_CONFIGURATION = 2,
    USB_DESCRIPTOR_TYPE_STRING = 3,
    USB_DESCRIPTOR_TYPE_INTERFACE = 4,
    USB_DESCRIPTOR_TYPE_ENDPOINT = 5,
    USB_DESCRIPTOR_TYPE_INTERFACE_POWER = 8,
    USB_DESCRIPTOR_TYPE_OTG = 9,
    USB_DESCRIPTOR_TYPE_DEBUG = 10,
    USB_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION = 11,
    USB_DESCRIPTOR_TYPE_BOS = 15,
    USB_DESCRIPTOR_TYPE_DEVICE_CAPABILITY = 16,
    USB_DESCRIPTOR_TYPE_HUB = 41,
    USB_DESCRIPTOR_TYPE_SUPERSPEED_USB_ENDPOINT_COMPANION = 48,
};

typedef struct usb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubclass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} OBOS_PACK usb_device_descriptor;

typedef struct usb_configuration_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} OBOS_PACK usb_configuration_descriptor;

typedef struct usb_descriptor_header {
    uint8_t bLength;
    uint8_t bDescriptorType;
} OBOS_PACK usb_descriptor_header;

typedef struct usb_hub_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bNbrPorts;
    uint16_t wHubCharacteristics;
    uint8_t bPowerOnGood;
    uint8_t bHubContrCurrent;
    uint8_t removeable_device_bmp[]; // size=bNbrPorts bits
} OBOS_PACK usb_hub_descriptor;

#define usb_next_descriptor(x) ((usb_descriptor_header*)(((uint8_t*)(x))[0] + (uintptr_t)(x)))

typedef struct usb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubclass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} OBOS_PACK usb_interface_descriptor;

typedef struct usb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    // bit 7: direction, OUT=0, IN=1
    // bits 0-3: endpoint number
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    // bits 0-10: max packet size
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} OBOS_PACK usb_endpoint_descriptor;

typedef union {
    struct {
        uint8_t class;
        uint8_t subclass;
        uint8_t protocol;
    };
    uint32_t hid;
} usb_hid;

typedef struct usb_hub_info
{
    uint8_t port_count;
    uint8_t tt_think_time;
    uint8_t parent_slot_id;
    uint32_t route_string;
    bool mtt : 1;
} usb_hub_info;

typedef struct usb_ctlr_ioctl_slot_allocate {
    // Output parameters
    uint8_t slot;
    uint32_t address;
    
    uint8_t port_number;
    uint32_t route_string;

    bool is_hub : 1;

    usb_hub_info hub_info;
} usb_ctlr_ioctl_slot_allocate;

// TODO: Will this translate easily to EHCI+UHCI?
// (does this *need* to translate easily to EHCI+UHCI)

typedef enum usb_trb_type {
    USB_TRB_NORMAL,
    // only legal for endpoint zero (control)
    USB_TRB_CONTROL,
    USB_TRB_ISOCH,
    USB_TRB_NOP,
    // NOTE: Should be done in both directions
    // NOTE: Is always invalid for the control endpoint.
    USB_TRB_CONFIGURE_ENDPOINT,
    // Configures a hub
    USB_TRB_CONFIGURE_HUB,
} usb_trb_type;

// argp is a usb_ctlr_ioctl_slot_allocate*
// desc is the handle in usb_controller
#define IOCTL_USB_CTLR_ALLOCATE_SLOT 0x8501

typedef enum usb_endpoint_type {
    USB_ENDPOINT_CONTROL = 0,
    USB_ENDPOINT_ISOCH,
    USB_ENDPOINT_BULK,
    USB_ENDPOINT_INTERRUPT,
} usb_endpoint_type;

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
            // idk
            struct physical_region* regions;
            size_t nRegions;
            // TODO: Return status in some way from the status stage TRB (xhci-only)? 
        } setup;
        struct {
            usb_endpoint_type endpoint_type;
            uint16_t max_packet_size;
            uint16_t max_burst_size;
            usb_hub_info hub_info;
            bool is_hub : 1;
            bool deconfigure : 1;
        } configure_endpoint;
        usb_hub_info configure_hub;
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
    usb_hid hid; // not initialized by the driver

    uint32_t address;
    uint8_t slot;
    uint8_t port;

    uint8_t speed;

    bool usb3 : 1;
} usb_device_info;

typedef struct usb_endpoint {
    struct usb_dev_desc* dev;

    uint8_t endpoint_number;
    // direction is false for OUT (IRP_WRITE), and true for IN (IRP_READ)
    bool direction : 1;
    usb_endpoint_type type;

    usb_endpoint_descriptor descriptor;

    LIST_NODE(usb_endpoint_list, struct usb_endpoint) node;
} usb_endpoint;
typedef LIST_HEAD(usb_endpoint_list, struct usb_endpoint) usb_endpoint_list;
LIST_PROTOTYPE(usb_endpoint_list, struct usb_endpoint, node);

typedef LIST_HEAD(usb_devices, struct usb_dev_desc) usb_devices;
LIST_PROTOTYPE(usb_devices, struct usb_dev_desc, node);
typedef struct usb_dev_desc {
    shared_ptr ptr;
    
    struct usb_dev_desc* parent;
    struct usb_controller* controller;
    
    usb_device_info info;

    bool attached : 1;
    bool is_hub : 1;

    event on_detach;

    // Reserved for use by controller drivers
    void* drv_ptr;
    // Reserved for use by device drivers
    void* dev_ptr;
    // driver_id*
    void* drv;

    struct {
        usb_hub_info info;
        thread* worker;
        usb_hub_descriptor *descriptor;
    } hub;

    // NOT IN ORDER!
    usb_endpoint_list endpoints;

    struct {
        uint8_t configuration_id;
        uint8_t configuration_idx;
    } configuration;

    usb_devices children;
    mutex children_lock;

    LIST_NODE(usb_devices, struct usb_dev_desc) node;
} usb_dev_desc;

typedef struct usb_controller {
    void* handle;
    struct driver_header* hdr;

    usb_devices ports;
    mutex ports_lock;
    
    LIST_NODE(usb_controller_list, struct usb_controller) node;
} usb_controller;
typedef LIST_HEAD(usb_controller_list, struct usb_controller) usb_controller_list;
LIST_PROTOTYPE(usb_controller_list, struct usb_controller, node);

extern usb_controller_list Drv_USBControllers;
extern mutex Drv_USBControllersLock;

OBOS_EXPORT obos_status Drv_USBControllerRegister(void* handle, struct driver_header* header, usb_controller** out);

// parent is nullable
OBOS_EXPORT obos_status Drv_USBPortAttached(usb_controller* ctlr, const usb_device_info* info, usb_dev_desc** desc, usb_dev_desc* parent);
OBOS_EXPORT obos_status Drv_USBPortPostAttached(usb_controller* ctlr, usb_dev_desc* desc);
OBOS_EXPORT obos_status Drv_USBPortDetached(usb_controller* ctlr, usb_dev_desc* desc);

// USB Device Drivers should call this when
// they attach themselves to a port.
// drv_id is struct driver_id*
OBOS_EXPORT OBOS_NODISCARD obos_status Drv_USBDriverAttachedToPort(usb_dev_desc* desc, void* drv_id);

// req is struct irp*
OBOS_EXPORT obos_status Drv_USBIRPSubmit(usb_dev_desc* desc, void* req);
// req is struct irp**
// dir is false for OUT (IRP_WRITE), and true for IN (IRP_READ)
OBOS_EXPORT obos_status Drv_USBIRPSubmit2(usb_dev_desc* desc, void** req, const usb_irp_payload* payload, bool dir);
OBOS_EXPORT obos_status Drv_USBIRPWait(usb_dev_desc* desc, void* req);
OBOS_EXPORT obos_status Drv_USBSynchronousOperation(usb_dev_desc* desc, const usb_irp_payload* payload, bool dir);

// Bits 24-31: The topmost hub number
uint32_t Drv_USBMakeRouteString(usb_dev_desc* desc);
char* Drv_USBMakePhysicalLocationString(usb_dev_desc* desc);

obos_status Drv_USBHubAttached(usb_dev_desc* desc);