/*
 * oboskrnl/vfs/irp.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/limits.h>
#include <vfs/vnode.h>

#include <driver_interface/header.h>

#include <locks/event.h>

enum irp_op {
    IRP_READ,
    IRP_WRITE,
};
typedef struct irp {
    // Set when the operation is complete.
    // The lifetime of the pointed object is completely controlled
    // by the driver, but needs to be alive until the event is set.
    // EVENT_NOTIFICATION.
    event *evnt;
    union {
        void *buff;
        const void* cbuff;
    };
    void* drvData;
    size_t refs;
    uoff_t blkOffset;
    size_t blkCount;
    union {
        size_t nBlkRead;
        size_t nBlkWritten;
    };
    dev_desc desc;
    vnode *vn;
    // NOTE: Call finalize_irp, if it exists, before checking this variable.
    obos_status status;
    // If dryOp is true, then no bytes should be read/written, but
    // evnt should still be set when blkCount bytes can be read/written.
    bool dryOp : 1;
    enum irp_op op : 1;
} irp;
// desc can be nullptr if request->desc can be implied from vn.
OBOS_EXPORT obos_status VfsH_IRPSubmit(irp* request, const dev_desc* desc);
OBOS_EXPORT obos_status VfsH_IRPBytesToBlockCount(vnode* vn, size_t nBytes, size_t *out);
OBOS_EXPORT obos_status VfsH_IRPWait(irp* request);
OBOS_EXPORT obos_status VfsH_IRPSignal(irp* request, obos_status status);
OBOS_EXPORT void VfsH_IRPRef(irp* request);
OBOS_EXPORT void VfsH_IRPUnref(irp* request);
OBOS_EXPORT irp* VfsH_IRPAllocate();