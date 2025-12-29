/*
 * drivers/generic/libps2/keyboard.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <stdarg.h>

#include <irq/irql.h>

#include <mm/page.h>
#include <mm/pmm.h>

#include <irq/dpc.h>

#include <locks/event.h>
#include <locks/wait.h>

#include <vfs/keycode.h>

#include <allocators/base.h>

#include "keyboard.h"
#include "scancode_tables.h"
#include "controller.h"
#include "detect.h"

static void signal_ring_buffer_dpc(dpc* d, void* userdata)
{
    OBOS_UNUSED(d);
    ps2_ringbuffer* buff = userdata;
    Core_EventSet(&buff->e, true);
}

static void keyboard_ready(ps2_port* port, uint8_t scancode)
{
    ps2k_data* data = port->pudata;
    if (!data->initialized)
        return;

    // A command response.
    if (scancode == 0xfa || scancode == 0xfe)
        return;
    
    if (scancode == 0xe0)
    {
        data->processing_extended = true;
        return;
    }
    if (scancode == 0xf0 && data->set == 2)
    {
        data->processing_release = true;
        return;
    }

    const uint8_t scancode_raw = (data->set == 1 ? (scancode & 0x7f) : scancode) - (data->processing_extended ? 0x10 : 0);
    const keycode *raw_code = nullptr;
    bool released = data->set == 1 ? (scancode & BIT(7)) : data->processing_release;
    data->processing_release = false;

    if (data->processing_extended)
    {
        data->processing_extended = false;
        if (scancode_raw >= ((data->set == 2) ? sizeof(set2_keycode_extended)/sizeof(keycode) : sizeof(set1_keycode_extended)/sizeof(keycode)))
            return; // funy
        raw_code = data->set == 1 ? &set1_keycode_extended[scancode_raw] : &set2_keycode_extended[scancode_raw];
    }
    else 
    {
        if (scancode_raw >= ((data->set == 2) ? sizeof(set2_keycode_normal)/sizeof(keycode) : sizeof(set1_keycode_normal)/sizeof(keycode)))
            return; // funy
        raw_code = data->set == 1 ? &set1_keycode_normal[scancode_raw] : &set2_keycode_normal[scancode_raw];
    }

    switch (SCANCODE_FROM_KEYCODE(*raw_code))
    {
        case SCANCODE_CTRL:
            data->ctrl = !released;
            break;
        case SCANCODE_ALT:
            data->alt = !released;
            break;
        case SCANCODE_SHIFT:
            data->shift = !released;
            break;
        case SCANCODE_FN:
            data->fn = !released;
            break;
        case SCANCODE_SUPER_KEY:
            data->super_key = !released;
            break;
        default: break;
    }

    bool changed_status = false;

    if (MODIFIERS_FROM_KEYCODE(*raw_code) & CAPS_LOCK && released)
    {
        data->caps_lock = !data->caps_lock;
        changed_status = true;
    }
    if (MODIFIERS_FROM_KEYCODE(*raw_code) & NUM_LOCK && released)
    {
        data->num_lock = !data->num_lock;
        changed_status = true;
    }

    if (changed_status)
    {
        uint8_t led_state = 0;
        if (data->num_lock)
            led_state |= BIT(1);
        if (data->caps_lock)
            led_state |= BIT(2);
        // TODO: Scroll lock.
        PS2_SendCommand(port, 0xed, 1, led_state);
    }
    
    if (SCANCODE_FROM_KEYCODE(*raw_code) == SCANCODE_UNKNOWN)
        return;

    keycode code = *raw_code;
    KEYCODE_ADD_MODIFIER(code, released ? KEY_RELEASED : 0);
    KEYCODE_ADD_MODIFIER(code, data->ctrl ? CTRL : 0);
    KEYCODE_ADD_MODIFIER(code, data->alt ? ALT : 0);
    KEYCODE_ADD_MODIFIER(code, data->fn ? FN : 0);
    KEYCODE_ADD_MODIFIER(code, data->shift ? SHIFT : 0);
    KEYCODE_ADD_MODIFIER(code, data->caps_lock ? CAPS_LOCK : 0);
    KEYCODE_ADD_MODIFIER(code, data->num_lock ? NUM_LOCK : 0);
    KEYCODE_ADD_MODIFIER(code, data->super_key ? SUPER_KEY : 0);

    // OBOS_Debug("Got key %s (0x%02x), modifiers 0x%02x\n", OBOS_ScancodeToString[SCANCODE_FROM_KEYCODE(code)], SCANCODE_FROM_KEYCODE(code), MODIFIERS_FROM_KEYCODE(code));
    PS2_RingbufferAppendKeycode(&data->input, code, false);
    
    data->dpc.userdata = &data->input;
    CoreH_InitializeDPC(&data->dpc, signal_ring_buffer_dpc, 0);
}

static ps2k_data keyboard_data_buf[2];

static obos_status read_code(void* handle, keycode* out, bool block)
{
    ps2k_handle* hnd = handle;
    if (hnd->magic != PS2K_HND_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = hnd->port;
    ps2k_data* data = port->pudata;

    if (hnd->in_ptr == data->input.out_ptr)
    {
        if (!Core_EventGetState(port->data_ready_event) && !block)
            return OBOS_STATUS_WOULD_BLOCK;

        obos_status status = Core_WaitOnObject(WAITABLE_OBJECT(*port->data_ready_event));
        if (obos_is_error(status))
            return status;
    }

    return PS2_RingbufferFetchKeycode(&data->input, &hnd->in_ptr, out);
}

static obos_status get_readable_count(void* handle, size_t* nReadable)
{
    ps2k_handle* hnd = handle;
    if (hnd->magic != PS2K_HND_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2_port* port = hnd->port;
    ps2k_data* data = port->pudata;
    *nReadable = (data->input.out_ptr - hnd->in_ptr);
    return OBOS_STATUS_SUCCESS;
}

static obos_status make_handle(struct ps2_port* port, void** handle)
{
    ps2k_data* data = port->pudata;
    if (data->ps2k_magic != PS2K_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    ps2k_handle* hnd = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(ps2k_handle), nullptr);
    hnd->magic = PS2K_HND_MAGIC_VALUE;
    hnd->port = port;
    hnd->in_ptr = data->input.out_ptr;
    *handle = hnd;
    return OBOS_STATUS_SUCCESS;
}
static obos_status close_handle(struct ps2_port* port, void* handle)
{
    ps2k_data* data = port->pudata;
    if (data->ps2k_magic != PS2K_MAGIC_VALUE || ((ps2k_handle*)handle)->magic != PS2K_HND_MAGIC_VALUE)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Free(OBOS_KernelAllocator, handle, sizeof(ps2k_handle));
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION void PS2_InitializeKeyboard(ps2_port* port)
{
    OBOS_ASSERT(Core_GetIrql() < IRQL_DISPATCH);
    OBOS_Log("PS/2: Initializing PS/2 Keyboard on channel %c\n", port->second ? '2' : '1');
    // PS2_MaskChannelIRQs(port->second, true);
    irql oldIrql = Core_RaiseIrql(IRQL_PS2);

    // port->suppress_irqs = true;

    port->pudata = &keyboard_data_buf[port->second];
    ps2k_data* data = port->pudata;
    data->ps2k_magic = PS2K_MAGIC_VALUE;
    data->port = port;
    data->initialized = false;

    port->data_ready = keyboard_ready;

    uint8_t res = PS2_SendCommand(port, 0xff, 0);
    if (res != 0xfa)
    {
        Core_LowerIrql(oldIrql);
        return;
    }
    int retries = 0;
    up:
    res = PS2_DeviceRead(1024, nullptr);
    if (res == 0xff && retries++ < 5)
        goto up;

    if (res != 0xAA)
    {
        OBOS_Warning("PS/2: While resetting PS/2 keyboard: Got 0x%02x instead of 0xaa (test success code). Aborting initialization\n", res);
        Core_LowerIrql(oldIrql);
        return;
    }

    PS2_SendCommand(port, 0xf5, 0);

	// Keys need to held for 250 ms before repeating, and they repeat at a rate of 30 hz (33.33333 ms).
    res = PS2_SendCommand(port, 0xf3, 1, 0x00);
    if (res != PS2_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }

    // Clear keyboard LEDs.
    res = PS2_SendCommand(port, 0xed, 1, 0x0);
    if (res != PS2_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }
    
    Core_LowerIrql(oldIrql);

    PS2_FlushInput();

    port->suppress_irqs = true;

    uint8_t set = 0;
#define DEFAULT_SET 2
#define ALTERNATE_SET 1
    // Try putting the keyboard into scancode set #2 by default, if that doesn't work (it sends RESEND), put it into scancode set #1
    set = DEFAULT_SET;
    bool found = false;
    while (!found)
    {
        bool retried = false;
        res = PS2_SendCommand(port, 0xf0, 1, set);
        if (res == PS2_RESEND && !retried)
        {
            retried = true;
            continue;
        }
        if (set == ALTERNATE_SET && res == PS2_RESEND)
        {
            set = 0;
            break;
        }
        if (res == PS2_RESEND)
            set = ALTERNATE_SET;
        else
            found = true;
    }
    if (!set)
    {
        OBOS_Error("PS/2: Could not put the keyboard into a defined scancode set (tried sets one and two, neither were recognized).\n");
        return;
    }

    // Enable scanning.
    // res = PS2_SendCommand(port, 0xf4, 0);
    // if (res != PS2_ACK)
    // {
    //     Core_LowerIrql(oldIrql);
    //     return;
    // }

    // 'true' is not an error, see PS2_StartKeyboard
    port->suppress_irqs = true;

    data->set = set;

    data->input.e = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    port->data_ready_event = &data->input.e;
    port->read_code = read_code;
    port->make_handle = make_handle;
    port->close_handle = close_handle;
    port->get_readable_count = get_readable_count;

    OBOS_Log("PS/2: Successfully initialized keyboard on channel %c\n", port->second ? '2' : '1');
    OBOS_Debug("PS/2 Keyboard is using scancode set %d\n", data->set);
    data->initialized = true;
    PS2_RingbufferInitialize(&data->input, false);

    port->type = PS2_DEV_TYPE_KEYBOARD;
    port->id[3] = port->type;
    make_handle(port, &port->default_handle);
    port->blk_size = sizeof(keycode);
}

void PS2_StartKeyboard(ps2_port* port)
{
    uint8_t res = PS2_SendCommand(port, 0xf4, 0);
    if (res != PS2_ACK)
        return;

    port->suppress_irqs = false;
}

obos_status PS2_RingbufferInitialize(ps2_ringbuffer* buff, bool mouse)
{
    memzero(buff, sizeof(*buff));

    buff->e = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    buff->size = OBOS_PAGE_SIZE;
    if (!mouse)
        buff->nElements = buff->size/sizeof(keycode);
    else
        buff->nElements = buff->size/sizeof(mouse_packet);

    page* phys = MmH_PgAllocatePhysical(false, false);
    if (!phys)
        return OBOS_STATUS_NOT_ENOUGH_MEMORY;
    buff->buff = MmS_MapVirtFromPhys(phys->phys);
    buff->out_ptr = 0;
    buff->handle_count = 0;

    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferAppendKeycode(ps2_ringbuffer* buff, keycode code, bool signal)
{
    buff->keycodes[buff->out_ptr++ % buff->nElements] = code;
    if (signal)
        Core_EventSet(&buff->e, true);
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferFetchKeycode(const ps2_ringbuffer* buff, size_t* in_ptr, keycode* code)
{
    if (*in_ptr == buff->out_ptr)
        return OBOS_STATUS_EOF;
    *code = buff->keycodes[(*in_ptr)++ % buff->nElements];
    // *in_ptr = *in_ptr % buff->nElemen.ts; // sanitize it for the user
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferAppendMousePacket(ps2_ringbuffer* buff, mouse_packet pckt, bool signal)
{
    buff->mouse_packets[buff->out_ptr++ % buff->nElements] = pckt;
    if (signal)
        Core_EventSet(&buff->e, true);
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferFetchMousePacket(const ps2_ringbuffer* buff, size_t* in_ptr, mouse_packet* pckt)
{
    if (*in_ptr == buff->out_ptr)
        return OBOS_STATUS_EOF;
    *pckt = buff->mouse_packets[(*in_ptr)++ % buff->nElements];
    // *in_ptr = *in_ptr % buff->nElements; // sanitize it for the user
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferFree(ps2_ringbuffer* buff)
{
    memset(buff->buff, 0xcc, buff->size);
    uintptr_t phys = MmS_UnmapVirtFromPhys(buff->buff);
    page what = {.phys=phys};
    Core_MutexAcquire(&Mm_PhysicalPagesLock);
    page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    Core_MutexRelease(&Mm_PhysicalPagesLock);
    MmH_DerefPage(pg);
    memset(buff, 0xcc, sizeof(*buff));
    return OBOS_STATUS_SUCCESS;
}