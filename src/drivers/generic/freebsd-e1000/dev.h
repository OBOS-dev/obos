#pragma once

#include <int.h>

#include <vfs/vnode.h>

#include <e1000/e1000_hw.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/dpc.h>

#include <locks/event.h>

#include <mm/page.h>

#include <driver_interface/pci.h>

#include <utils/list.h>

#define RX_QUEUE_SIZE (OBOS_PAGE_SIZE / sizeof(union e1000_rx_desc_extended))
#define TX_QUEUE_SIZE (32)

typedef struct e1000_frame {
    void* buff;
    size_t size;
    size_t refs;
    LIST_NODE(e1000_frame_list, struct e1000_frame) node;
} e1000_frame;
typedef LIST_HEAD(e1000_frame_list, e1000_frame) e1000_frame_list;
LIST_PROTOTYPE(e1000_frame_list, e1000_frame, node);

typedef struct e1000_device {
    struct e1000_hw hw;
    struct e1000_osdep osdep;
    char* interface_name;
    vnode* vn;

    pci_resource* irq_res;
    irq irq;
    uint32_t icr;
    dpc dpc;
    dpc dpc_tx;

    uintptr_t rx_ring;
    uintptr_t rx_ring_buffers[RX_QUEUE_SIZE];
    page* rx_ring_phys_pg;
    event rx_evnt;
    
    uintptr_t tx_ring;
    page* tx_ring_phys_pg;
    size_t tx_index;
    event tx_done_evnt;

    size_t refs;

    e1000_frame_list rx_frames;
} e1000_device;

#define E1000_HANDLE_MAGIC 0xe100070d
typedef struct e1000_handle {
    uint32_t magic;
    e1000_device* dev;
    e1000_frame* rx_curr;
    size_t rx_off;
} e1000_handle;

#if OBOS_IRQL_COUNT == 16
#	define IRQL_E1000 (7)
#elif OBOS_IRQL_COUNT == 8
#	define IRQL_E1000 (3)
#elif OBOS_IRQL_COUNT == 4
#	define IRQL_E1000 (2)
#elif OBOS_IRQL_COUNT == 2
#	define IRQL_E1000 (0)
#else
#	error Funny business.
#endif

void e1000_init_rx(e1000_device* dev);
void e1000_init_tx(e1000_device* dev);
event* e1000_tx_packet(e1000_device* dev, const void* buffer, size_t size, bool dry);

void e1000_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);
bool e1000_check_irq_callback(struct irq* i, void* userdata);