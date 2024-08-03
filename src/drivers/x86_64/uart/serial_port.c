/*
 * drivers/x86_64/uart/serial_port.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <allocators/base.h>

#include <arch/x86_64/asm_helpers.h>

#include <arch/x86_64/ioapic.h>

#include <scheduler/dpc.h>

#include <driver_interface/header.h>

#include <irq/irql.h>

#include "serial_port.h"
#include "scheduler/thread.h"


void append_to_buffer_char(buffer* buf, char what)
{
    size_t index = buf->szBuf++;
    if (index >= buf->nAllocated)
    {
        buf->nAllocated += 4; // Reserve 4 more bytes.
        buf->buf = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, buf->buf, buf->nAllocated*sizeof(*buf->buf), nullptr);
        OBOS_ASSERT(buf->buf);
    }
    buf->buf[index] = what;
}
void append_to_buffer_str_len(buffer* buf, const char* what, size_t strlen)
{
    // Reserve enough bytes to prevent a bunch of allocations from being made.
    if (buf->nAllocated < (buf->szBuf + strlen))
    {
        buf->nAllocated += ((strlen + 3) & ~3); // Reserve strlen more bytes.
        buf->buf = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, buf->buf, buf->nAllocated*sizeof(*buf->buf), nullptr);
        OBOS_ASSERT(buf->buf);
    }
    char ch = what[0];
    for (size_t i = 0; i < strlen; ch = what[++i])
        append_to_buffer_char(buf, ch);
}
void append_to_buffer_str(buffer* buf, const char* what)
{
    append_to_buffer_str_len(buf, what, strlen(what));
}
char pop_from_buffer(buffer* buf)
{
    if (!buf->buf || !buf->szBuf)
        return 0;
    char ret = buf->buf[buf->offset];
    buf->szBuf--;
    buf->offset++;
    if ((buf->nAllocated - buf->szBuf) >= 4)
    {
        buf->nAllocated -= (buf->nAllocated - buf->szBuf);
        OBOS_ASSERT(buf->nAllocated == buf->szBuf);
        char* newbuf = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, buf->nAllocated*sizeof(*buf->buf), nullptr);
        memcpy(newbuf, buf->buf + buf->offset, buf->szBuf);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf->buf, buf->nAllocated + 4);
        buf->buf = newbuf;
        buf->offset = 0;
    }
    return ret;
}
void free_buffer(buffer* buf)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, buf->buf, buf->nAllocated*sizeof(*buf->buf));
    buf->buf = nullptr;
    buf->szBuf = buf->nAllocated = 0;
}
void flush_out_buffer(serial_port* port)
{
    while((inb(port->port_base + LINE_STATUS) & BIT(5)) && port->out_buffer.szBuf)
        outb(port->port_base + IO_BUFFER, pop_from_buffer(&port->out_buffer));
}

obos_status open_serial_connection(serial_port* port, uint32_t baudRate, data_bits dataBits, stop_bits stopbits, parity_bit parityBit, dev_desc* connection)
{
    if (!port || !baudRate)
        return OBOS_STATUS_INVALID_ARGUMENT;
    uint16_t divisor = 115200 / baudRate;
    if (divisor == 0)
        return OBOS_STATUS_INTERNAL_ERROR;
    // NOTE: You still should allow initialization of the port, even if it was deduced to be faulty,
    // as it seems as if disconnected serial ports fail in the same way.
    irql oldIrql = Core_RaiseIrql(IRQL_COM_IRQ);
    // Disable serial IRQs (temporarily)
    outb(port->port_base + IRQ_ENABLE, 0);
    outb(port->port_base + LINE_CTRL, 0x80 /* LINE_CTRL.DLAB */);
    outb(port->port_base + DIVISOR_LOW_BYTE, divisor & 0xff);
    outb(port->port_base + DIVISOR_HIGH_BYTE, divisor >> 8);
    outb(port->port_base + LINE_CTRL, dataBits | stopbits | parityBit);
    while (inb(port->port_base + LINE_STATUS) & BIT(0))
        inb(port->port_base + IO_BUFFER);
    // Enter loop back mode.
    outb(port->port_base + MODEM_CTRL, 0x1B /* RTS+Out1+Out2+Loop */);
    outb(port->port_base + IO_BUFFER, 0xde);
    if (inb(port->port_base + IO_BUFFER) != 0xde)
    {
	    // OBOS_Debug("Port COM%d is faulty or disconnected.", port->com_port);
        port->isFaulty = true; // bruh
        Core_LowerIrql(oldIrql);
        return OBOS_STATUS_INTERNAL_ERROR;
    }
    // Enter normal transmission mode.
    port->isFaulty = false;
    outb(port->port_base + FIFO_CTRL, 0x07 /* FIFO Enabled, IRQ when one byte is received, other flags */);
    outb(port->port_base + MODEM_CTRL, 0xF /* DTR+RTS+OUT2+OUT1*/);
    outb(port->port_base + IRQ_ENABLE, 1);
    Arch_IOAPICMaskIRQ(port->gsi, false);
    Core_LowerIrql(oldIrql);
    *connection = (dev_desc)port;
    return OBOS_STATUS_SUCCESS;
}

bool workPending = false;
void com_dpc_handler(struct dpc* work, void* userdata)
{
    serial_port *port = userdata;
    uint8_t lineStatusRegister = inb(port->port_base + LINE_STATUS);
    if (lineStatusRegister & BIT(0))
    {
        // Receive all the avaliable data.
        irql oldIrql = Core_SpinlockAcquireExplicit(&port->in_buffer.lock, IRQL_COM_IRQ, false);
        while (inb(port->port_base + LINE_STATUS) & BIT(0))
            append_to_buffer_char(&port->in_buffer, inb(port->port_base + IO_BUFFER));
        Core_SpinlockRelease(&port->in_buffer.lock, oldIrql);
    }
    if (lineStatusRegister & BIT(5))
    {
        // Send all the data in the output buffer.
        irql oldIrql = Core_SpinlockAcquireExplicit(&port->out_buffer.lock, IRQL_COM_IRQ, false);
        flush_out_buffer(port);
        Core_SpinlockRelease(&port->out_buffer.lock, oldIrql);
    }
    CoreH_FreeDPC(work);
    workPending = false;
}
void com_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(frame);
    OBOS_UNUSED(oldIrql);
    if (!workPending)
    {
        // TODO: Is this a good idea?
        dpc *work = CoreH_AllocateDPC(nullptr);
        work->userdata = userdata;
        CoreH_InitializeDPC(work, com_dpc_handler, Core_DefaultThreadAffinity);
        workPending = true; 
    }
}
bool com_check_irq_callback(struct irq* i, void* userdata)
{
    serial_port* port = (serial_port*)userdata;
    return inb(port->port_base + LINE_STATUS) != 0 && !port->isFaulty;
}
void com_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata)
{
    OBOS_UNUSED(i);
    OBOS_UNUSED(from);
    serial_port* port = (serial_port*)userdata;
    obos_status status = Arch_IOAPICMapIRQToVector(port->gsi, 0, true, TriggerModeEdgeSensitive);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "IOAPIC: Could not unmap GSI %d. Status: %d\n", port->gsi, status);
    status = Arch_IOAPICMapIRQToVector(port->gsi, to->id+0x20, true, TriggerModeEdgeSensitive);
    if (obos_is_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "IOAPIC: Could not map GSI %d. Status: %d\n", port->gsi, status);
}