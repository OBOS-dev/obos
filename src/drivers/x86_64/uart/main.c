/*
 * drivers/x86_64/uart/main.c
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>

#include <stdarg.h>

#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <scheduler/thread.h>

#include <uacpi/resources.h>
#include <uacpi/utilities.h>

#include <allocators/base.h>

#include <irq/irq.h>
#include <irq/irql.h>

#include <arch/x86_64/ioapic.h>

#include <arch/x86_64/asm_helpers.h>

#include <locks/spinlock.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include "serial_port.h"

#include <uacpi/types.h>
#include <uacpi_libc.h>

driver_id* this_driver;
serial_port* serialPorts = nullptr;
size_t nSerialPorts = 0;

void cleanup()
{
    for (size_t i = 0; i < nSerialPorts; i++)
    {
        size_t sz = 0;
        OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, serialPorts[i].user_name, &sz);
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, serialPorts[i].user_name, sz);
        free_buffer(&serialPorts[i].in_buffer);
        free_buffer(&serialPorts[i].out_buffer);
    }
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, serialPorts, sizeof(*serialPorts)*nSerialPorts);
}
obos_status get_blk_size(dev_desc ign, size_t* sz)
{
    OBOS_UNUSED(ign);
    if (!sz)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *sz = 1;
    return OBOS_STATUS_SUCCESS;
}
obos_status get_max_blk_count(dev_desc ign1, size_t* ign2)
{
    OBOS_UNUSED(ign1);
    OBOS_UNUSED(ign2);
    return OBOS_STATUS_INVALID_OPERATION;
}
obos_status query_user_readable_name(dev_desc what, const char** name)
{
    if (!what || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;
    serial_port* port = (serial_port*)what;
    if (port->user_name)
        *name = port->user_name;
    else
    {
        size_t sz = snprintf(nullptr, 0, "COM%d", port->com_port);
        char* buf = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sz+1, nullptr);
        buf[sz] = 0;
        snprintf(buf, sz+1, "COM%d", port->com_port);
        port->user_name = buf;
        *name = port->user_name;
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* udata), void* udata)
{
    for (size_t i = 0; i < nSerialPorts; i++)
    {
        switch (cb((dev_desc)&serialPorts[i], 1, -1, udata))
        {
            case ITERATE_DECISION_CONTINUE:
                continue;
            case ITERATE_DECISION_STOP:
                return OBOS_STATUS_SUCCESS;
        }
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    OBOS_UNUSED(blkOffset);
    serial_port* port = (serial_port*)desc;
    if (!port || !buf || !blkCount)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t i = 0;
    while(port->in_buffer.szBuf < blkCount)
        ;
    irql oldIrql = Core_SpinlockAcquireExplicit(&port->in_buffer.lock, IRQL_COM_IRQ, false);
    const size_t initialBufSize = port->in_buffer.szBuf;
    for (; i < blkCount && i < initialBufSize; i++)
        ((char*)buf)[i] = pop_from_buffer(&port->in_buffer);
    Core_SpinlockRelease(&port->in_buffer.lock, oldIrql);
    if (nBlkRead)
        *nBlkRead = i;
    return OBOS_STATUS_SUCCESS;
}
obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(blkOffset);
    serial_port* port = (serial_port*)desc;
    if (!port || !buf || !blkCount)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irql oldIrql = Core_SpinlockAcquireExplicit(&port->out_buffer.lock, IRQL_COM_IRQ, false);
    for (size_t i = 0; i < blkCount; i++)
    {
        size_t spin = 0;
        const size_t threshold = 100000;
        while(spin++ < threshold && !(inb(port->port_base + LINE_STATUS) & BIT(5)))
            pause();
        if (spin >= threshold)
        {
            // buffer the writes after it hangs for too long.
            append_to_buffer_str_len(&port->out_buffer, &((char*)buf)[i], blkCount);
            break;
        }
        outb(port->port_base + IO_BUFFER, ((char*)buf)[i]);
    }
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    Core_SpinlockRelease(&port->out_buffer.lock, oldIrql);
    return OBOS_STATUS_SUCCESS;
}
obos_status ioctl(size_t nParameters, uint64_t request, ...);
obos_status ioctl_var(size_t nParameters, uint64_t request, va_list list);
__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_PIPE_STYLE_DEVICE|DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI|DRIVER_HEADER_HAS_VERSION_FIELD,
    .acpiId.nPnpIds = 2,
    .acpiId.pnpIds = {
        "PNP0500", // Standard PC COM port
        "PNP0501", // 16550A-compatible COM port
    },
    .ftable = {
        .driver_cleanup_callback = cleanup,
        .ioctl = ioctl,
        .ioctl_var = ioctl_var,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
    },
    .driverName = "COM Driver",
    .version = 1,
    .uacpi_init_level_required = UACPI_INIT_LEVEL_NAMESPACE_INITIALIZED
};

uacpi_resource_iteration_decision resource_iterator(void *user, uacpi_resource *resource)
{
    serial_port* curr = (serial_port*)user;
    switch (resource->type)
    {
        case UACPI_RESOURCE_TYPE_IRQ:
        {
            curr->gsi = resource->irq.irqs[0];
            break;
        }
        case UACPI_RESOURCE_TYPE_IO:
        {
            curr->port_base = resource->io.minimum;
            curr->port_top = curr->port_base + resource->io.length;
            break;
        }
        default: break;
    }
    return UACPI_RESOURCE_ITERATION_CONTINUE;
}
static uacpi_ns_iteration_decision match_uart(void *user, uacpi_namespace_node *node)
{
    OBOS_UNUSED(user);
    uacpi_resources* resources = nullptr;
    uacpi_status ret = uacpi_get_current_resources(node, &resources);
    if (uacpi_unlikely_error(ret))
    {
        OBOS_Error("Could not retrieve resources! Status: %s\n", uacpi_status_to_string(ret));
        return UACPI_NS_ITERATION_DECISION_NEXT_PEER;
    }
    serialPorts = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, serialPorts, (++nSerialPorts)*sizeof(serial_port), nullptr);
    memzero(&serialPorts[nSerialPorts - 1], sizeof(*serialPorts));
    serialPorts[nSerialPorts - 1].com_port = nSerialPorts;
    uacpi_for_each_resource(resources, resource_iterator, &serialPorts[nSerialPorts - 1]);
    uacpi_free_resources(resources);
    
    // OBOS_Debug("COM%d is from IO address 0x%x - 0x%x, and is on GSI %d.\n", 
    //     serialPorts[nSerialPorts-1].com_port,
    //     serialPorts[nSerialPorts-1].port_base,
    //     serialPorts[nSerialPorts-1].port_top,
    //     serialPorts[nSerialPorts-1].gsi
    // );

    return UACPI_NS_ITERATION_DECISION_CONTINUE;
}

obos_status ioctl_var(size_t nParameters, uint64_t request, va_list list)
{
    obos_status status = OBOS_STATUS_INVALID_IOCTL;
    switch (request) 
    {
        case IOCTL_OPEN_SERIAL_CONNECTION:
        {
            if (nParameters != IOCTL_OPEN_SERIAL_CONNECTION_PARAMETER_COUNT)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            uint8_t id = (uint8_t)va_arg(list, uint32_t);
            if (id > nSerialPorts)
            {
                status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            serial_port* port = &serialPorts[id-1];
            if (port->com_port != id)
            {
                status = OBOS_STATUS_INTERNAL_ERROR;
                break;
            }
            uint32_t baudRate = va_arg(list, uint32_t);
            data_bits dataBits = va_arg(list, data_bits);
            stop_bits stopBits = va_arg(list, stop_bits);
            parity_bit parityBit = va_arg(list, parity_bit);
            dev_desc* connection = va_arg(list, dev_desc*);
            status = open_serial_connection(
                port, 
                baudRate,
                dataBits,
                stopBits,
                parityBit,
                connection);
            break;
        }
    }
    return status;
}
obos_status ioctl(size_t nParameters, uint64_t request, ...)
{
    obos_status status = OBOS_STATUS_INVALID_IOCTL;
    va_list list;
    va_start(list, request);
    status = ioctl_var(nParameters, request, list);
    va_end(list);
    return status;
}

OBOS_PAGEABLE_FUNCTION driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    // Find the keyboard driver.
    uacpi_find_devices("PNP0500", match_uart, nullptr);
    uacpi_find_devices("PNP0501", match_uart, nullptr);
    // For each COM port, make an IRQ object.
    for (size_t i = 0; i < nSerialPorts; i++)
    {
        serial_port* port = &serialPorts[i];
        obos_status status = OBOS_STATUS_SUCCESS;
        port->irq_obj = Core_IrqObjectAllocate(&status);
        if (obos_is_error(status))
        {
            OBOS_Warning("Could not allocate irq object for COM%d. Status: %d.\n", port->com_port, status);
            continue;
        }
        port->irq_obj->handler = com_irq_handler;
        port->irq_obj->irqChecker = com_check_irq_callback;
        port->irq_obj->moveCallback = com_irq_move_callback;
        port->irq_obj->handlerUserdata = port;
        port->irq_obj->irqCheckerUserdata = port;
        port->irq_obj->irqMoveCallbackUserdata = port;
        status = Core_IrqObjectInitializeIRQL(port->irq_obj, IRQL_COM_IRQ, true, true);
        if (obos_is_error(status))
        {
            OBOS_Warning("Could not initialize irq object for COM%d. Status: %d.\n", port->com_port, status);
            continue;
        }

        // Register the GSI.
        status = Arch_IOAPICMapIRQToVector(port->gsi, port->irq_obj->vector->id+0x20, true, TriggerModeEdgeSensitive);
        if (obos_is_error(status))
        {
            OBOS_Warning("Could not initialize GSI for COM%d. Status: %d.\n", port->com_port, status);
            continue;
        }

        vnode* vn = Drv_AllocateVNode(this_driver, (uintptr_t)port, 0, nullptr, VNODE_TYPE_CHR);
        const char* dev_name = nullptr;
        query_user_readable_name(vn->desc, &dev_name);
        OBOS_Debug("%*s: Registering serial port at %s%s\n", uacpi_strnlen(this_driver->header.driverName, 64), this_driver->header.driverName, OBOS_DEV_PREFIX, dev_name);
        Drv_RegisterVNode(vn, dev_name);
    }
        return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false,.context=nullptr};
}
