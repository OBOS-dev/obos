/*
 * drivers/generic/xhci/xhci.h
 *
 * Copyright (c) 2026 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <struct_packing.h>

#include <driver_interface/pci.h>
#include <driver_interface/usb.h>

#include <locks/event.h>
#include <locks/mutex.h>

#include <mm/page.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>

#include <utils/tree.h>

typedef struct xhci_registers {
    uint8_t caplength;
    uint8_t resv1;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
    char resv2[];
} OBOS_PACK xhci_registers;

typedef struct xhci_port_registers {
    uint32_t port_sc;
    uint32_t port_pmsc;
    uint32_t port_li;
    uint32_t port_hlpmc;
} OBOS_PACK xhci_port_registers;

typedef struct xhci_op_registers {
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t pagesize;
    uint64_t resv1;
    uint32_t dnctrl;
    uint64_t crcr;
    uint64_t resv2[2];
    uint64_t dcbaap;
    uint32_t config;
    uint8_t resv3[0x3c4];
    xhci_port_registers ports[];
} OBOS_PACK xhci_op_registers;

typedef struct xhci_interrupter_registers {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t resv;
    uint64_t erstba;
    uint64_t erdp;
} OBOS_PACK xhci_interrupter_registers;

typedef struct xhci_runtime_registers {
    uint32_t mfindex;
    uint32_t resv[7];
    xhci_interrupter_registers interrupters[1024];
} OBOS_PACK xhci_runtime_registers;

enum {
    PORTSC_CCS = BIT(0),
    PORTSC_PED = BIT(1),
    PORTSC_OCA = BIT(3),
    PORTSC_PR = BIT(4),
    PORTSC_PLS = 0x1E0,
    PORTSC_PP = BIT(9),
    PORTSC_ROS = 0x3C00,
    PORTSC_PIC = 0xC000,
    PORTSC_LWS = BIT(16),
    PORTSC_CSC = BIT(17),
    PORTSC_PEC = BIT(18),
    PORTSC_WRC = BIT(19),
    PORTSC_OCC = BIT(20),
    PORTSC_PRC = BIT(21),
    PORTSC_PLC = BIT(22),
    PORTSC_CEC = BIT(23),
    PORTSC_CAS = BIT(24),
    PORTSC_WCE = BIT(25),
    PORTSC_WDE = BIT(26),
    PORTSC_WOE = BIT(27),
    PORTSC_DR = BIT(30),
    PORTSC_WPR = BIT(31),
};

enum {
    USB3_PORT_PMSC_U1_TIMEOUT_MASK = 0xFF,
    USB3_PORT_PMSC_U2_TIMEOUT_MASK = 0xFF00,
    USB3_PORT_PMSC_FLA = BIT(16),
};

enum {
    USB2_PORT_PMSC_L1S_MASK = 0x7,
    USB2_PORT_PMSC_RWE = BIT(3),
    USB2_PORT_PMSC_BESL_MAK = 0xf0,
    USB2_PORT_PMSC_L1_DEV_SLOT_MASK = 0xff00,
    USB2_PORT_PMSC_HLE = BIT(16),
    USB2_PORT_PMSC_PORT_TEST_CTRL_MASK = 0xF0000000,
};

enum {
    USB3_PORTLI_LINK_ERROR_COUNT_MASK = 0xffff,
    USB3_PORTLI_RLC_MASK = 0xF0000,
    USB3_PORTLI_TLC_MASK = 0xF00000,
};

enum {
    USBCMD_RUN = BIT(0),
    USBCMD_RESET = BIT(1),
    USBCMD_INTE = BIT(2),
    USBCMD_HSEE = BIT(3),
    USBCMD_LHCRST = BIT(7),
    USBCMD_CSS = BIT(8),
    USBCMD_CRS = BIT(9),
    USBCMD_EWE = BIT(10),
    USBCMD_EU3S = BIT(11),
    USBCMD_CME = BIT(13),
    USBCMD_ETE = BIT(14),
    USBCMD_TSC_EN = BIT(15),
    USBCMD_VTIOE = BIT(16),
};

enum {
    USBSTS_HCH = BIT(0),
    USBSTS_HSE = BIT(2),
    USBSTS_EINT = BIT(3),
    USBSTS_PCD = BIT(4),
    USBSTS_SSS = BIT(8),
    USBSTS_RSS = BIT(9),
    USBSTS_SRE = BIT(10),
    USBSTS_CNR = BIT(11),
    USBSTS_HCE = BIT(12),
};

enum {
    CRCR_RCS = BIT(0),
    CRCR_CS = BIT(1),
    CRCR_CA = BIT(2),
    CRCR_CRR = BIT(3),
    CRCR_CRP_MASK = 0xFFFFFFFFFFFFFFC0,
};

enum {
    DCBAAP_MASK = 0xFFFFFFFFFFFFFFC0,
};

enum {
    OP_CONFIG_MAX_SLOTS_EN_MASK = 0xff,
    OP_CONFIG_U3E = BIT(8),
    OP_CONFIG_CIE = BIT(9),
};

// Transfer TRBs

typedef struct xhci_normal_trb {
    uint64_t dbp;

    // Bits 0-16: Length
    // Bits 17-21: TD Size
    // Bits 22-31: Interrupter Target
    uint32_t length_td_size;

    // Bits 0-9: Flags
    // Bits 10-15: TRB Type
    uint16_t flags_type;
    // Bit zero: direction, rest is reserved, only applicable if is a data stage TRB
    uint16_t dir_resv;
} OBOS_PACK xhci_normal_trb, xhci_data_stage_trb;

typedef struct xhci_setup_stage_trb {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;

    uint16_t wIndex;
    uint16_t wLength;

    uint16_t length; // always 8!
    // Bits 16-21: Reserved
    // Bits 22-32: Interrupter Target
    uint16_t td_size_target;

    // Bits 0-9: Flags
    // Bits 10-15: TRB Type
    uint16_t flags_type;
    uint8_t trt; // only bits 0 and 1, rest is reserved
    uint8_t resv;
} OBOS_PACK xhci_setup_stage_trb;

typedef struct xhci_status_stage_trb {
    uint32_t resv1[2];

    uint16_t resv2;
    // Bits 22:31
    uint16_t interrupter_target;

    // Bits 0-9: Flags
    // Bits 10-15: TRB Type
    uint16_t flags_type;
    // Bit zero: direction, rest is reserved
    uint16_t dir_resv;
} OBOS_PACK xhci_status_stage_trb;

typedef struct xhci_isoch_trb {
    uint64_t dbp;

    uint16_t length;
    // Bits 16-21: TD Size/TBC (If ETC=0, TD Size, otherwise, is the TBC)
    // Bits 22-32: Interrupter Target
    uint16_t td_size_target;

    // Bits 0-9: Flags
    // Bits 10-15: TRB Type
    uint16_t flags_type;
    // Bits 0-3: TLBPC (Transfer Last Burst Packet Count)
    // Bits 4-15: Frame Id
    // Bit 16: Start Iosch ASAP (SIA)
    uint16_t tlbpc_frame_sia;
} OBOS_PACK xhci_isoch_trb;

typedef struct xhci_nop_trb {
    uint16_t resv1[4+1];

    // Bits 22:31 of dword 2
    uint16_t interrupter_target;

    // Bits 0-5: Flags
    // Bits 6-9: Resv
    // Bits 10-15: TRB Type
    uint16_t flags_type;
    uint16_t resv3;
} OBOS_PACK xhci_nop_trb;

// Event TRBs

typedef struct xhci_transfer_event_trb {
    uint64_t trbp;

    // Bits 0-23: TRB Transfer Length
    // Bits 24-31: Completion Code
    uint32_t trb_transfer_length_code;

    // Bit 0: Cycle Bit
    // Bit 1: Reserved
    // Bit 2: Event Data
    // Bits 3-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-20: Endpoint ID
    // Bits 21-23: Reserved
    // Bits 24-31: Slot ID
    uint32_t dw3;
} OBOS_PACK xhci_transfer_event_trb;

typedef struct xhci_commmand_completion_event_trb {
    uint64_t ctrbp; // aligned to 16 bytes

    // Bits 0-23: Command Completion Parameter
    // Bits 24-31: Completion Code
    uint32_t trb_transfer_length_code;

    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: VF ID
    // Bits 24-31: Slot ID
    uint32_t dw3;
} OBOS_PACK xhci_commmand_completion_event_trb;

typedef struct xhci_port_status_change_event_trb {
    // Bits 0-23: Reserved
    // Bits 24-31: Port ID
    uint32_t dw0;
    
    uint32_t dw1; // reserved
    
    // Bits 0-23: Reserved
    // Bits 24-31: Completion Code
    uint32_t dw2;

    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-31: Reserved
    uint32_t dw3;    
} OBOS_PACK xhci_port_status_change_event_trb;

typedef struct xhci_bandwith_request_event_trb {
    uint8_t resv[11];
    
    // Completion code
    uint8_t completion_code;

    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: Reserved
    // Bits 24-31: Slot ID 
    uint32_t dw3;
} OBOS_PACK xhci_bandwith_request_event_trb;

typedef struct xhci_doorbell_event_trb {
    uint8_t db_reason;
    uint16_t resv1;
    uint8_t resv2;
    
    uint32_t resv3;

    // Bits 0-23: Reserved
    // Bits 24-31: Completion Code
    uint32_t dw2;

    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: VF ID
    // Bits 24-31: Slot ID 
    uint32_t dw3;    
} OBOS_PACK xhci_doorbell_event_trb;

typedef struct xhci_host_ctlr_event_trb {
    uint8_t resv[11];
    
    // Completion code
    uint8_t completion_code;

    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-31: Reserved
    uint32_t dw3;
} OBOS_PACK xhci_host_ctlr_event_trb;

typedef struct xhci_device_notification_event_trb {
    // Bits 0-3: Reserved
    // Bits 4-7: Notification type
    // Bits 8-63: Device notification data pointer.
    uint64_t dndp_notification_type; // aligned to 256 bytes

    uint8_t resv1[3];
    uint8_t completion_code;
    
    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: Reserved
    // Bits 24-31: Slot ID
    uint32_t dw3;
} OBOS_PACK xhci_device_notification_event_trb;

// Command TRBs

typedef struct xhci_nop_command_trb {
    uint32_t resv[3];
    
    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: Slot Type
    // Bits 24-31: Reserved
    uint32_t dw3;
} xhci_nop_command_trb, xhci_enable_slot_command_trb;

typedef struct xhci_disable_slot_command_trb {
    uint32_t resv[3];
    
    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-23: Reserved
    // Bits 24-31: Slot ID
    uint32_t dw3;
} xhci_disable_slot_command_trb;

typedef struct xhci_address_device_command_trb {
    uint64_t icp; // aligned to 16 bytes
    
    uint32_t resv;
    
    // Bit 0: Cycle Bit
    // Bits 1-8: Reserved
    // Bit 9: Block Set Address Request (BSR) Bit
    // Bit 9: Deconfigure (DC) Bit
    // Bits 10-15: TRB Type
    // Bits 16-23: Reserved
    // Bits 24-31: Slot ID
    uint32_t dw3;
} xhci_address_device_command_trb, xhci_evaluate_context_command_trb, xhci_configure_endpoint_command_trb;

typedef struct xhci_reset_endpoint_command_trb {
    uint32_t resv[3];
    
    // Bit 0: Cycle Bit
    // Bits 1-8: Reserved
    // Bit 9: Transfer State Preserve (TSP) Bit
    // Bit 9: Reserved, for reset device.
    // Bits 10-15: TRB Type
    // Bits 16-23: Reserved
    // Bits 24-31: Slot ID
    uint32_t dw3;
} xhci_reset_endpoint_command_trb, xhci_reset_device_comamnd_trb;

typedef struct xhci_stop_endpoint_command_trb {
    uint32_t resv[3];
    
    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-20: Endpoint ID
    // Bits 21-22: Reserved
    // Bit 23: Suspend (SP) Bit
    // Bits 24-31: Slot ID
    uint32_t dw3;
} xhci_stop_endpoint_command_trb;

typedef struct xhci_get_port_bandwith_command_trb {
    uint64_t pbcp; // aligned to 16 bytes

    uint32_t resv;
    
    // Bit 0: Cycle Bit
    // Bits 1-9: Reserved
    // Bits 10-15: TRB Type
    // Bits 16-19: Dev Speed
    // Bits 20-23: Reserved
    // Bits 24-31: Hub Slot ID
    uint32_t dw3;
} xhci_get_port_bandwith_command_trb;

#define XHCI_GET_TRB_TYPE(trb) ((((uint32_t*)(trb))[3] >> 10) & 0x3f)
#define XHCI_SET_TRB_TYPE(trb, type) ({ (((uint32_t*)(trb))[3]) |= (((type) & 0x3f) << 10); (type); })
#define XHCI_GET_COMPLETION_CODE(trb) (((((uint32_t*)(trb))[2]) >> 24) & 0xff)
#define XHCI_GET_COMPLETION_PARAMETER(trb) ((((uint32_t*)(trb))[2]) & 0xffffff)
// NOTE: is transfer length - transferred bytes count
#define XHCI_GET_TRB_TRANSFER_LENGTH(trb) ((((uint32_t*)(trb))[2]) & 0xffffff)

enum {
    XHCI_TRB_NORMAL = 1,
    XHCI_TRB_SETUP_STAGE,
    XHCI_TRB_DATA_STAGE,
    XHCI_TRB_STATUS_STAGE,
    XHCI_TRB_ISOCH,
    XHCI_TRB_LINK,
    XHCI_TRB_EVENT_DATA,
    XHCI_TRB_NOP,
    XHCI_TRB_ENABLE_SLOT_COMMAND,
    XHCI_TRB_DISABLE_SLOT_COMMAND,
    XHCI_TRB_ADDRESS_DEVICE_COMMAND,
    XHCI_TRB_CONFIGURE_ENDPOINT_COMMAND,
    XHCI_TRB_EVALUATE_CONTEXT_COMMAND,
    XHCI_TRB_RESET_ENDPOINT_COMMAND,
    XHCI_TRB_STOP_ENDPOINT_COMMAND,
    XHCI_TRB_SET_TR_DEQUEUE_POINTER_COMMAND,
    XHCI_TRB_RESET_DEVICE_COMMAND,
    XHCI_TRB_FORCE_EVENT_COMMAND,
    XHCI_TRB_NEGOTIATE_BANDWITH_COMMAND,
    XHCI_TRB_SET_LATENCY_TOLERANCE_VALUE_COMMAND,
    XHCI_TRB_GET_PORT_BANDWITH_COMMAND,
    XHCI_TRB_FORCE_HEADER_COMMAND,
    XHCI_TRB_NOP_COMMAND,
    XHCI_TRB_GET_EXTENDED_PROPERTY_COMMAND,
    XHCI_TRB_SET_EXTENDED_PROPERTY_COMMAND,
    XHCI_TRB_TRANSFER_EVENT = 32,
    XHCI_TRB_COMMAND_COMPLETION_EVENT,
    XHCI_TRB_PORT_STATUS_EVENT,
    XHCI_TRB_DOORBELL_EVENT,
    XHCI_TRB_HOST_CONTROLLER_EVENT,
    XHCI_TRB_DEVICE_NOTIFICATION_EVENT,
    XHCI_TRB_MFINDEX_WRAP_EVENT,
};

union xhci_device_context_element {
    uint64_t scratchpad_array_base;
    uint64_t device_context_base;
};

typedef struct xhci_endpoint_context {
    uint16_t flags1;
    uint8_t interval;
    uint8_t max_esit_payload_high;
    uint8_t flags2;
    uint8_t max_burst_size;
    uint16_t max_packet_size;
    uint64_t tr_dequeue_pointer; // bit zero: dcs, aligned to 16 bytes
    uint16_t average_trb_length;
    uint16_t max_esit_payload_low;
    uint32_t resv[3];
} OBOS_PACK xhci_endpoint_context;

typedef struct xhci_slot_context {
    uint32_t dw0;
    uint32_t dw1;
    uint32_t dw2;
    uint32_t dw3;
    uint32_t resv[4];
} OBOS_PACK xhci_slot_context;

typedef struct xhci_input_context {
    struct {
        uint32_t drop_context;
        uint32_t add_context;
        uint32_t resv1[5];
        uint8_t conf_value;
        uint8_t iface_num;
        uint8_t alt_setting;
        uint8_t resv2;
    } OBOS_PACK icc;
    char device_context[];
} OBOS_PACK xhci_input_context;

typedef struct xhci_event_ring_segment_table_entry {
    uint64_t rsba; // ring segment base address
    uint16_t rss; // ring segment size
    uint16_t resv[3];
} OBOS_PACK xhci_event_ring_segment_table_entry;

typedef struct xhci_slot {
    struct {
        struct {
            void* virt;
            size_t len;
            page* pg;
        } buffer;
        uint64_t enqueue_ptr;
        uint64_t dequeue_ptr;
    } trb_ring[31];
    
    uint32_t* doorbell;
    uint32_t address;
    usb_dev_desc* desc;

    uint8_t port_id;
    bool allocated : 1;
} xhci_slot;

typedef struct xhci_inflight_trb {
    uintptr_t ptr;
    uint64_t* dequeue_ptr;
    uint32_t* resp;
    uint8_t resp_length; // in dwords
    event evnt;
    RB_ENTRY(xhci_inflight_trb) node;
} xhci_inflight_trb;

struct xhci_inflight_trb_array {
    uint32_t count;
    uint32_t index;
    xhci_inflight_trb* itrbs[];
};

static inline int cmp_inflight_trb(xhci_inflight_trb *lhs, xhci_inflight_trb *rhs)
{
    return (lhs->ptr < rhs->ptr) ? -1 : ((lhs->ptr == rhs->ptr) ? 0 : 1);
}

typedef RB_HEAD(xhci_trbs_inflight, xhci_inflight_trb) xhci_trbs_inflight;
RB_PROTOTYPE(xhci_trbs_inflight, xhci_inflight_trb, node, cmp_inflight_trb);

typedef struct xhci_device {
    pci_device* dev;

    union {
        volatile void* base;
        volatile xhci_registers* cap_regs;
    };

    volatile xhci_op_registers* op_regs; 
    volatile xhci_runtime_registers* rt_regs; 

    pci_resource* pci_bar;
    pci_resource* pci_irq;

    irq irq;
    dpc irq_dpc;
    bool handling_irq : 1;

    // Same bitfield as usbsts, but only the interrupt status bits
    uint32_t irqsts;

    bool did_bios_handoff : 1;
    bool has_64bit_support : 1;
    bool port_power_control_supported : 1;
    bool hccparams1_csz : 1;
    uint16_t xecp;
    uint16_t max_slots;

    struct {
        void* virt;
        size_t len;
        page* pg;
        uintptr_t enqueue_ptr;
        uintptr_t dequeue_ptr;
    } command_ring;
    struct {
        void* virt;
        size_t len;
        size_t nEntries;
        page* pg;
        bool ccs : 1;
    } event_ring;

    struct {
        union {
            void* virt;
            union xhci_device_context_element* base;
        };
        size_t len;
        page* pg;
    } device_context_array;

    xhci_slot *slots;
    uint8_t port_to_slot_id[255];

    xhci_trbs_inflight trbs_inflight;
    mutex trbs_inflight_lock;

    usb_controller* ctlr;
    
    struct xhci_device *next, *prev;
} xhci_device;

static inline void* get_xhci_endpoint_context(xhci_device* dev, void* device_context, int dci)
{
    return (void*)(((uintptr_t)device_context) + dci * (dev->hccparams1_csz ? 64 : 32));
}

#ifndef INIT_C
extern struct {
#else
struct {
#endif
    xhci_device *head, *tail;
    size_t nNodes;
} g_devices;

void xhci_probe_bus(pci_bus* bus);
obos_status xhci_initialize_device(xhci_device* dev);
obos_status xhci_reset_device(xhci_device* dev);

obos_status xhci_slot_initialize(xhci_device* dev, uint8_t slot, uint8_t port);
obos_status xhci_slot_free(xhci_device* dev, uint8_t slot);

enum {
    XHCI_DIRECTION_OUT,
    XHCI_DIRECTION_IN,
};
typedef bool xhci_direction;

obos_status xhci_trb_enqueue_slot(xhci_device* dev, uint8_t slot, uint8_t endpoint, xhci_direction direction, uint32_t* trb, xhci_inflight_trb** itrb, bool doorbell);
obos_status xhci_trb_enqueue_command(xhci_device* dev, uint32_t* trb, xhci_inflight_trb** itrb, bool doorbell);

// Rings the doorbell of the target slot, endpoint, and direction
void xhci_doorbell_slot(xhci_slot* slot, uint8_t endpoint, xhci_direction direction /* true for OUT, false for IN */);
// Rings the control doorbell of the host controller.
void xhci_doorbell_control(xhci_device* dev);

void* xhci_get_device_context(xhci_device* dev, uint8_t slot);

OBOS_WEAK bool xhci_irq_checker(irq*, void*);
OBOS_WEAK void xhci_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);

// true indicates a successful wait, false indicates timeout
bool poll_bit_timeout(volatile uint32_t *field, uint32_t mask, uint32_t expected, uint32_t us_timeout);

#define xhci_append_device(dev_) \
do {\
    typeof(dev_) _dev = (dev_);\
    if (!g_devices.head)\
        g_devices.head = _dev;\
    if (g_devices.tail)\
        g_devices.tail->next = _dev;\
    _dev->prev = g_devices.tail;\
    g_devices.tail = _dev;\
    g_devices.nNodes++;\
} while(0)

#if OBOS_IRQL_COUNT == 16
#	define IRQL_XHCI (9)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_XHCI (4)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_XHCI (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_XHCI (0)
#else
#	error Funny business.
#endif