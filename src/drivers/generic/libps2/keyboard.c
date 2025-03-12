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

#include <locks/event.h>

#include <vfs/keycode.h>

#include "keyboard.h"
#include "scancode_tables.h"
#include "controller.h"

static OBOS_PAGEABLE_FUNCTION uint8_t send_command(ps2_port* port, uint8_t cmd, size_t nArgs, ...);

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
        printf("set caps lock to %s\n", data->caps_lock ? "true":"false");
    }
    if (MODIFIERS_FROM_KEYCODE(*raw_code) & NUM_LOCK && released)
    {
        data->num_lock = !data->num_lock;
        changed_status = true;
        printf("set num lock to %s\n", data->num_lock ? "true":"false");
    }

    if (changed_status)
    {
        uint8_t led_state = 0;
        if (data->num_lock)
            led_state |= BIT(1);
        if (data->caps_lock)
            led_state |= BIT(2);
        // TODO: Scroll lock.
        send_command(port, 0xed, 1, led_state);
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

    OBOS_Debug("Got key %s (0x%02x), modifiers 0x%02x\n", OBOS_ScancodeToString[SCANCODE_FROM_KEYCODE(code)], SCANCODE_FROM_KEYCODE(code), MODIFIERS_FROM_KEYCODE(code));
    PS2_RingbufferAppend(&data->input, code, true);
}

static OBOS_PAGEABLE_FUNCTION uint8_t send_command_impl(ps2_port* port, obos_status* status, uint8_t cmd, size_t nArgs, va_list list)
{
    PS2_DeviceWrite(port->second, cmd);
    for (size_t i = 0; i < nArgs; i++)
        PS2_DeviceWrite(port->second, va_arg(list, uint32_t) & 0xff);
    return PS2_DeviceRead(0xffff, status);
}

ps2k_data keyboard_data_buf[2];

static OBOS_PAGEABLE_FUNCTION uint8_t send_command(ps2_port* port, uint8_t cmd, size_t nArgs, ...)
{
    obos_status status = OBOS_STATUS_SUCCESS;
    va_list list;
    va_start(list, nArgs);
    uint8_t res = send_command_impl(port, &status, cmd, nArgs, list);
    if (obos_is_error(status))
    {
        OBOS_Warning("Timeout while waiting for a response from the PS/2 Keyboard. Aborting\n");
        res = PS2K_INVALID;
        goto done;
    }
    if (res == PS2K_ACK)
        goto ack;
    ack:
    done:
    va_end(list);
    return res;
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

    uint8_t res = send_command(port, 0xff, 0);
    if (res != 0xfa)
    {
        Core_LowerIrql(oldIrql);
        return;
    }
    res = PS2_DeviceRead(1024, nullptr);
    if (res != 0xAA)
    {
        OBOS_Warning("PS/2: While resetting PS/2 keyboard: Got 0x%02x instead of 0xaa (test success code). Aborting initialization\n", res);
        Core_LowerIrql(oldIrql);
        return;
    }

	// Keys need to held for 250 ms before repeating, and they repeat at a rate of 30 hz (33.33333 ms).
    res = send_command(port, 0xf3, 1, 0x00);
    if (res != PS2K_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }

    // Enable scanning.
    res = send_command(port, 0xf4, 0);
    if (res != PS2K_ACK)
    {
        Core_LowerIrql(oldIrql);
        return;
    }

    // Clear keyboard LEDs.
    res = send_command(port, 0xed, 1, 0x0);
    if (res != PS2K_ACK)
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
        res = send_command(port, 0xf0, 1, set);
        if (res == PS2K_RESEND && !retried)
        {
            retried = true;
            continue;
        }
        if (set == ALTERNATE_SET && res == PS2K_RESEND)
        {
            set = 0;
            break;
        }
        if (res == PS2K_RESEND)
            set = ALTERNATE_SET;
        else
            found = true;
    }
    if (!set)
    {
        OBOS_Error("PS/2: Could not put the keyboard into a defined scancode set (tried sets one and two, neither were recognized).\n");
        return;
    }

    port->suppress_irqs = false;

    data->set = set;
    OBOS_Log("PS/2: Successfully initialized keyboard on channel %c\n", port->second ? '2' : '1');
    OBOS_Debug("PS/2 Keyboard is using scancode set %d\n", data->set);
    data->initialized = true;
    PS2_RingbufferInitialize(&data->input);
}

obos_status PS2_RingbufferInitialize(ps2k_ringbuffer* buff)
{
    memzero(buff, sizeof(*buff));
    buff->e = EVENT_INITIALIZE(EVENT_NOTIFICATION);
    buff->size = OBOS_PAGE_SIZE;
    buff->nElements = buff->size/sizeof(keycode);
    page* phys = MmH_PgAllocatePhysical(false, false);
    if (!phys)
        return OBOS_STATUS_NOT_ENOUGH_MEMORY;
    buff->buff = MmS_MapVirtFromPhys(phys->phys);
    buff->out_ptr = 0;
    buff->handle_count = 0;
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferAppend(ps2k_ringbuffer* buff, keycode code, bool signal)
{
    buff->keycodes[buff->out_ptr++ % buff->nElements] = code;
    if (signal)
        Core_EventSet(&buff->e, true);
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferFetch(const ps2k_ringbuffer* buff, size_t* in_ptr, keycode* code)
{
    if (*in_ptr == buff->out_ptr)
        return OBOS_STATUS_EOF;
    *code = buff->keycodes[(*in_ptr)++ % buff->nElements];
    *in_ptr = *in_ptr % buff->nElements; // sanitize it for the user
    return OBOS_STATUS_SUCCESS;
}

obos_status PS2_RingbufferFree(ps2k_ringbuffer* buff)
{
    memset(buff->buff, 0xcc, buff->size);
    uintptr_t phys = MmS_UnmapVirtFromPhys(buff->buff);
    page what = {.phys=phys};
    page* pg = RB_FIND(phys_page_tree, &Mm_PhysicalPages, &what);
    MmH_DerefPage(pg);
    memset(buff, 0xcc, sizeof(*buff));
    return OBOS_STATUS_SUCCESS;
}