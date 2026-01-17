/*
 * drivers/generic/usb-keyboard/main.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <driver_interface/header.h>
#include <driver_interface/usb.h>
#include <driver_interface/driverId.h>

OBOS_WEAK obos_status get_blk_size(dev_desc desc, size_t* blkSize);
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_WEAK obos_status submit_irp(void*);
OBOS_WEAK obos_status finalize_irp(void*);
OBOS_WEAK obos_status ioctl(dev_desc what, uint32_t request, void* argp);
void driver_cleanup_callback() {}

driver_id* this_driver;

OBOS_WEAK void on_wake();
OBOS_WEAK void on_suspend();

obos_status on_usb_attach(usb_dev_desc* desc)
{
    // desc->ptr is already refed for us.
    OBOS_UNUSED(desc);
    obos_status status = Drv_USBDriverAttachedToPort(desc, this_driver);
    if (obos_is_success(status))
    {
        OBOS_Debug("usb-hid: device bound to driver\n");
        return OBOS_STATUS_SUCCESS;
    }
    return status;
}

obos_status on_usb_detach(usb_dev_desc* desc)
{
    OBOS_Debug("usb-hid: device removed\n");
    OBOS_SharedPtrUnref(&desc->ptr);
    return OBOS_STATUS_SUCCESS;
}

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|
             DRIVER_HEADER_FLAGS_DETECT_VIA_USB|
             DRIVER_HEADER_HAS_VERSION_FIELD|
             DRIVER_HEADER_FLAGS_USB_DO_NOT_CHECK_SUBCLASS,
    .usbHid = {
        .class = 0x03,
        .subclass = 0x00,
        .protocol = 0x00,
    },
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .on_wake = on_wake,
        .on_suspend = on_suspend,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,
        .on_usb_attach = on_usb_attach,
        .on_usb_detach = on_usb_detach,
    },
    .driverName = "USB HID Driver",
    .version=2,
    .uacpi_init_level_required = PCI_IRQ_UACPI_INIT_LEVEL
};

driver_init_status OBOS_DriverEntry(driver_id* this)
{
    this_driver = this;
    return (driver_init_status){.status=OBOS_STATUS_SUCCESS};
}