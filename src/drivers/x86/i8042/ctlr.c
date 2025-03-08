/*
 * drivers/x86/i8042/ctlr.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <locks/spinlock.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/x86_64/ioapic.h>
#include <arch/x86_64/asm_helpers.h>

#include "klog.h"
#include "ps2_structs.h"

struct ps2_ctlr_data PS2_CtlrData;

static void poll_status(uint8_t mask, uint8_t expected)
{
    while ((inb(PS2_CMD_STATUS) & mask) != expected)
        pause();
}
static uint8_t read_ctlr_status()
{
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_READ_RAM_CMD(0));
    poll_status(PS2_OUTPUT_BUFFER_FULL, 0);
    return inb(PS2_DATA);
}
static void write_ctlr_status(uint8_t ctlr_config)
{
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_WRITE_RAM_CMD(0));
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, ctlr_config);
}
static uint8_t read_ctlr_output_port()
{
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_READ_CTLR_OUT_BUFFER);
    poll_status(PS2_OUTPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
    return inb(PS2_DATA);
}

void ps2_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    ps2_port* port = (ps2_port*)userdata;
    obos_status status = Arch_IOAPICMapIRQToVector(port->gsi, 0, true, TriggerModeEdgeSensitive);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "IOAPIC: Could not unmap GSI %d. Status: %d\n", port->gsi, status);
    status = Arch_IOAPICMapIRQToVector(port->gsi, to->id+0x20, true, TriggerModeEdgeSensitive);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "IOAPIC: Could not map GSI %d. Status: %d\n", port->gsi, status);
}
bool ps2_irq_check_callback(struct irq* i, void* userdata)
{
    OBOS_UNUSED(i);
    ps2_port* port = (ps2_port*)userdata;
    return read_ctlr_output_port() & BIT(4 + (port->second ? 1 : 0));
}
void ps2_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i && frame && oldIrql);
    ps2_port* port = userdata;
    port->data_ready(PS2_DeviceRead(0xfffffff, nullptr));
}

obos_status PS2_InitializeController()
{    
    irql oldIrql = Core_RaiseIrql(IRQL_PS2);

    PS2_CtlrData.lock = Core_SpinlockCreate();

    // Disable devices.
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_DISABLE_PORT_ONE);    
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_DISABLE_PORT_TWO);

    // Run the PS/2 Controller self test.
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_TEST);    
    poll_status(PS2_OUTPUT_BUFFER_FULL, 0);
    uint8_t test_result = inb(PS2_CMD_STATUS);
    if (test_result != 0x55 /* test passed */)
    {
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_INTERNAL_ERROR;_
    }

    // Write the controller configuration byte.
    uint8_t ctlr_config = read_ctlr_status();
    ctlr_config &= ~(PS2_CTLR_CONFIG_PORT_ONE_IRQ|PS2_CTLR_CONFIG_PORT_TWO_IRQ|PS2_CTLR_CONFIG_PORT_ONE_TRANSLATION);
    write_ctlr_status(ctlr_config);

    // Determine if the controller is dual-channel.
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_ENABLE_PORT_TWO);
    ctlr_config = read_ctlr_status();
    PS2_CtlrData.dual_channel = ~ctlr_config & PS2_CTLR_CONFIG_PORT_TWO_CLOCK;

    // Run device tests.
    poll_status(PS2_INPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
    poll_status(PS2_INPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
    outb(PS2_CMD_STATUS, PS2_CTLR_TEST_PORT_ONE);
    poll_status(PS2_OUTPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
    test_result = inb(PS2_CMD_DATA);
    if (!test_result)
        PS2_CtlrData.ports[0].works = true;
    if (PS2_CtlrData.dual_channel)
    {
        poll_status(PS2_INPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
        outb(PS2_CMD_STATUS, PS2_CTLR_TEST_PORT_TWO);
        poll_status(PS2_OUTPUT_BUFFER_FULL, PS2_OUTPUT_BUFFER_FULL);
        test_result = inb(PS2_CMD_DATA);
        if (!test_result)
            PS2_CtlrData.ports[1].works = true;
    }

    if (!PS2_CtlrData.ports[0].works && !PS2_CtlrData.ports[1].works)
    {
        OBOS_Log("Found %d PS/2 ports, but neither devices work (self test failed). Aborting.\n", PS2_CtlrData.dual_channel ? 2 : 1);
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_NOT_FOUND; // No devices.
    }

    // Enable devices.
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_ENABLE_PORT_ONE);    
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    outb(PS2_CMD_STATUS, PS2_CTLR_ENABLE_PORT_TWO);

    // Write the controller configuration byte.
    ctlr_config = read_ctlr_status();
    ctlr_config |= PS2_CTLR_CONFIG_PORT_ONE_IRQ|PS2_CTLR_CONFIG_PORT_TWO_IRQ;
    write_ctlr_status(ctlr_config);

    PS2_CtlrData.ports[1].second = true;
    // Initialize the structs.
    for (int i = 0; i < 2; i++)
    {
        if (!PS2_CtlrData.ports[i].works)
            continue;
        PS2_CtlrData.ports[i].gsi = (i==0) ? 1 : 12 /* TODO: Use ACPI namespace? */;
        PS2_CtlrData.ports[i].irq = Core_IrqObjectAllocate(nullptr);
        PS2_CtlrData.ports[i].irq->irqMoveCallbackUserdata = PS2_CtlrData.ports+i;
        PS2_CtlrData.ports[i].irq->irqCheckerUserdata = PS2_CtlrData.ports+i;
        PS2_CtlrData.ports[i].irq->handlerUserdata = PS2_CtlrData.ports+i;
        PS2_CtlrData.ports[i].irq->moveCallback = ps2_irq_move_callback;
        PS2_CtlrData.ports[i].irq->irqChecker = ps2_irq_check_callback;
        PS2_CtlrData.ports[i].irq->handler = ps2_irq_handler;
        Core_IrqObjectInitializeIRQL(PS2_CtlrData.ports[i].irq, IRQL_PS2, true, false);
        Arch_IOAPICMapIRQToVector(PS2_CtlrData.ports[i].gsi, PS2_CtlrData.ports[i].irq->vector->id+0x20, PolarityActiveHigh, TriggerModeEdgeSensitive);
        Arch_IOAPICMaskIRQ(PS2_CtlrData.ports[i].gsi, false);
    }

    Core_LowerIrql(oldIrql);
    
    return OBOS_STATUS_SUCCESS;
}

void PS2_DeviceWrite(bool port_two, uint8_t val)
{
    irql oldIrql = Core_SpinlockAcquireExplicit(&PS2_CtlrData.lock, IRQL_PS2, false);
    poll_status(PS2_INPUT_BUFFER_FULL, 0);
    if (port_two)
        outb(PS2_CMD_STATUS, PS2_CTLR_WRITE_PORT_TWO);
    outb(PS2_DATA, val);
    Core_SpinlockRelease(&PS2_CtlrData.lock, oldIrql);
    
}
uint8_t PS2_DeviceRead(uint32_t spin_timeout, obos_status* status)
{
    size_t i = 0;
    for (; ((inb(PS2_CMD_STATUS) & PS2_OUTPUT_BUFFER_FULL) != PS2_OUTPUT_BUFFER_FULL) && i < spin_timeout; i++)
        pause();
    if (status) *status = OBOS_STATUS_SUCCESS;
    if ((inb(PS2_CMD_STATUS) & PS2_OUTPUT_BUFFER_FULL) != PS2_OUTPUT_BUFFER_FULL)
    {
        if (status) *status = OBOS_STATUS_TIMED_OUT;
        return 0xff;
    }
    return inb(PS2_DATA);
}