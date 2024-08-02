/*
 * drivers/x86_64/uart/serial_port.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>

#include <irq/irq.h>

#include <locks/spinlock.h>

typedef struct buffer
{
    char* buf;
    size_t offset;
    size_t szBuf;
    size_t nAllocated;
    spinlock lock;
} buffer;
void append_to_buffer_char(buffer* buf, char what);
void append_to_buffer_str(buffer* buf,const char* what);
void append_to_buffer_str_len(buffer* buf, const char* what, size_t strlen);
char pop_from_buffer(buffer* buf); // NOTE: Pops from beginning of buffer.
void free_buffer(buffer* buf);
typedef struct serial_port
{
    uint8_t com_port;
    char* user_name;
    
    uint16_t port_base;
    uint16_t port_top;
    
    uint8_t gsi;
    irq* irq_obj;

    buffer in_buffer;
    buffer out_buffer;

    bool isFaulty;
} serial_port;
void flush_out_buffer(serial_port* port);

enum uart_registers
{
    IO_BUFFER = 0,
    DIVISOR_LOW_BYTE = 0, // When FIFO_CTRL.DLAB = 1
    IRQ_ENABLE = 1,
    DIVISOR_HIGH_BYTE = 1, // When FIFO_CTRL.DLAB = 1
    INTERRUPT_IDENTIFICATION = 2,
    FIFO_CTRL = 2,
    LINE_CTRL = 3,
    MODEM_CTRL = 4,
    LINE_STATUS = 5,
    MODEM_STATUS = 6,
    SCRATCH = 7,
};
typedef enum parity_bit
{
    PARITYBIT_NONE,
    PARITYBIT_ODD = 0b10000,
    PARITYBIT_EVEN = 0b11000,
    PARITYBIT_MARK = 0b10100,
    PARITYBIT_SPACE = 0b11100,
} parity_bit;
typedef enum data_bits
{
    FIVE_DATABITS,
    SIX_DATABITS,
    SEVEN_DATABITS,
    EIGHT_DATABITS,
} data_bits;
typedef enum stop_bits
{
    ONE_STOPBIT,
    ONE_HALF_STOPBIT = 0b0100,
    TWO_STOPBIT = 0b0100,
} stop_bits;

// Put this in the middle; not too priortized, nor too unprioritized.
#define IRQL_COM_IRQ 8

enum
{
    // Returns: obos_status
    // Parameters:
    // uint8_t: Port
    // uint32_t: Baud rate
    // data_bits: The amount of data bits.
    // stop_bits: The amount of stop bits.
    // parity_bit: The parity bit.
    // dev_desc*: Device descriptor associated with the connection.
    IOCTL_OPEN_SERIAL_CONNECTION,

    IOCTL_OPEN_SERIAL_CONNECTION_PARAMETER_COUNT = 6,
};

obos_status open_serial_connection(serial_port* port, uint32_t baudRate, data_bits dataBits, stop_bits stopbits, parity_bit parityBit, dev_desc* connection);

void com_irq_handler(struct irq* i, interrupt_frame* frame, void* userdata, irql oldIrql);
bool com_check_irq_callback(struct irq* i, void* userdata);
void com_irq_move_callback(struct irq* i, struct irq_vector* from, struct irq_vector* to, void* userdata);