/*
 * drivers/generic/libps2/mouse.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>

#include <vfs/keycode.h>

#include <locks/event.h>

#include <irq/dpc.h>

#include <allocators/base.h>

#include "controller.h"
#include "ringbuffer.h"
#include "mouse.h"
#include "detect.h"

static ps2m_data mouse_data_buf[2];

static obos_status read_pckt(void* handle, mouse_packet* out, bool block)
{
    ps2m_handle* hnd = handle;
    if (hnd->magic != PS2M_HND_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = hnd->port;
    ps2m_data* data = port->pudata;

    if (hnd->in_ptr == data->packets.out_ptr)
    {
        if (!Core_EventGetState(port->data_ready_event) && !block)
            return OBOS_STATUS_WOULD_BLOCK;

        Core_WaitOnObject(WAITABLE_OBJECT(*port->data_ready_event));
    }

    return PS2_RingbufferFetchMousePacket(&data->packets, &hnd->in_ptr, out);
}

static obos_status get_readable_count(void* handle, size_t* nReadable)
{
    ps2m_handle* hnd = handle;
    if (hnd->magic != PS2M_HND_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = hnd->port;
    ps2m_data* data = port->pudata;
    *nReadable = (data->packets.out_ptr - hnd->in_ptr);
    return OBOS_STATUS_SUCCESS;
}

static obos_status make_handle(struct ps2_port* port, void** handle)
{
    ps2m_data* data = port->pudata;
    if (data->magic != PS2M_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2m_handle* hnd = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ps2m_handle), nullptr);
    hnd->magic = PS2M_HND_MAGIC_VALUE;
    hnd->port = port;
    hnd->in_ptr = data->packets.out_ptr;
    *handle = hnd;
    return OBOS_STATUS_SUCCESS;
}

static void set_mouse_rate(ps2_port* port, uint8_t rate)
{
    uint8_t res = 0;
    if ((res = PS2_SendCommand(port, 0xf3, 1, rate)) != 0xfa)
        OBOS_Warning("set_mouse_rate(%d) got 0x%02x instead of ACK from mouse.\n", rate, res);
}

static uint8_t get_id(ps2_port* port)
{
    uint8_t res = PS2_SendCommand(port, 0xf2, 0);
    if (res != 0xfa)
    {
        OBOS_Warning("get_id() got 0x%02x instead of ACK from mouse.\n", res);
        return res;
    }
    return PS2_DeviceRead(0x20000, nullptr);
}

static void dpc_hnd(dpc* d, void* udata)
{
    OBOS_UNUSED(d);
    Core_EventSet((event*)udata, false);
}
static void mouse_ready(ps2_port* port, uint8_t b)
{
    ps2m_data* data = port->pudata;
    data->raw_pckt[data->nReady++] = b;
    if (!ps2m_enough_data(data))
        return;
    data->nReady = 0;
    
    mouse_packet pckt = {};

    pckt.mb = data->basic_pckt.flags & PS2M_BM;
    pckt.lb = data->basic_pckt.flags & PS2M_BL;
    pckt.rb = data->basic_pckt.flags & PS2M_BR;
    if (data->b4b5_extension_enabled)
    {
        pckt.b4 = data->b5_pckt.flags2 & PS2M_FLAGS2_B4;
        pckt.b5 = data->b5_pckt.flags2 & PS2M_FLAGS2_B5;
        pckt.x = data->b5_pckt.x - (data->b5_pckt.flags & PS2M_XS ? 0x100 : 0);
        pckt.y = data->b5_pckt.y - (data->b5_pckt.flags & PS2M_YS ? 0x100 : 0);
        pckt.z = data->b5_pckt.flags2 & PS2M_FLAGS2_Z_MASK;
    }
    else if (data->z_axis_extension_enabled)
    {
        pckt.b4 = 0;
        pckt.b5 = 0;
        pckt.x = data->z_pckt.x - (data->z_pckt.flags & PS2M_XS ? 0x100 : 0);
        pckt.y = data->z_pckt.y - (data->z_pckt.flags & PS2M_YS ? 0x100 : 0);
        pckt.z = (data->z_pckt.z & 0x7) * (data->z_pckt.z & 0x8 ? -1 : 1);
    }
    else
    {
        pckt.b4 = 0;
        pckt.b5 = 0;
        pckt.z = 0;
        pckt.x = data->basic_pckt.x - (data->basic_pckt.flags & PS2M_XS ? 0x100 : 0);
        pckt.y = data->basic_pckt.y - (data->basic_pckt.flags & PS2M_YS ? 0x100 : 0);
    }

    PS2_RingbufferAppendMousePacket(&data->packets, pckt, false);
    data->dpc.userdata = (struct event*)port->data_ready_event;
    CoreH_InitializeDPC(&data->dpc, dpc_hnd, Core_DefaultThreadAffinity);
}

void PS2_InitializeMouse(ps2_port* port)
{
    OBOS_Log("PS/2: Initializing PS/2 Mouse on channel %c\n", port->second ? '2' : '1');

    port->pudata = &mouse_data_buf[port->second];
    ps2m_data* data = port->pudata;
    data->magic = PS2M_MAGIC_VALUE;
    data->port = port;
    data->initialized = false;

    port->suppress_irqs = true;
    // irql oldIrql = Core_RaiseIrql(IRQL_PS2);

    uint8_t res = PS2_SendCommand(port, 0xff, 0);
    if (res != 0xfa)
        return;

    res = PS2_DeviceRead(0xffff, nullptr);
    if (res != 0xAA)
    {
        OBOS_Warning("PS/2: While resetting PS/2 Mouse: Got 0x%02x instead of 0xaa (test success code). Aborting initialization\n", res);
        return;
    }
    // Discard the next byte (mouse id)
    PS2_DeviceRead(0xffff, nullptr);

    // for (volatile bool b = true; b; )
    //     ;

    PS2_SendCommand(port, 0xf5, 0);

    // Do fnuy shit to tryn get a z axis, and buttons 4 and 5
    set_mouse_rate(port, 200);
    set_mouse_rate(port, 100);
    set_mouse_rate(port, 80);
    uint8_t id = get_id(port);
    if (id != 3)
    {
        printf("oh btw id=0x%02x\n", id);
        goto premature_finish;
    }
    data->z_axis_extension_enabled = true;
    set_mouse_rate(port, 200);
    set_mouse_rate(port, 200);
    set_mouse_rate(port, 80);
    id = get_id(port);
    if (id != 4)
        goto premature_finish;
    data->b4b5_extension_enabled = true;

    premature_finish:
    
    set_mouse_rate(port, 60);

    // Core_LowerIrql(oldIrql);

    // the lack of suppress_irqs=false is not a bug,
    // see PS2_StartMouse

    port->data_ready_event = &data->packets.e;
    port->read_mouse_packet = read_pckt;
    port->make_handle = make_handle;
    port->get_readable_count = get_readable_count;
    port->type = PS2_DEV_TYPE_MOUSE;
    port->id[3] = port->type;
    make_handle(port, &port->default_handle);
    port->blk_size = sizeof(mouse_packet);
    data->initialized = true;
    PS2_RingbufferInitialize(&data->packets, true);
    
    OBOS_Log("PS/2: Successfully initialized mouse on channel %c\n", port->second ? '2' : '1');

    if (data->z_axis_extension_enabled)
        OBOS_Log("PS/2: Z Axis Extension Enabled\n");
    if (data->b4b5_extension_enabled)
        OBOS_Log("PS/2: Buttons 4&5 Extension Enabled\n");
    port->data_ready = mouse_ready;
}
void PS2_StartMouse(ps2_port* port)
{
    PS2_SendCommand(port, 0xf4, 0);
    port->suppress_irqs = false;
}

void PS2_FreeMouse(ps2_port* port);