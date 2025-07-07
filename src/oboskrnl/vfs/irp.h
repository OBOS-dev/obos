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

// Before using data from the IRP, make sure to call VfsH_IRPWait on the IRP.
// Do not try to manually wait on the IRP, as there is tedious logic, and 
// getting it wrong can cause weird bugs.
// If you do, then good luck,
// and godspeed.
typedef struct irp {
    // Set when the operation is complete.
    // The lifetime of the pointed object is completely controlled
    // by the driver, but needs to be alive until the event is set.
    // If set to nullptr, there is data immediately available.
    // Always check if status != OBOS_STATUS_IRP_RETRY before calling finalize_irp.
    // EVENT_NOTIFICATION.
    event* volatile evnt;
    // If not nullptr, should be called by the IRP owner after waiting for the event.
    void(*on_event_set)(struct irp* irp);
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
    obos_status status;
    // If dryOp is true, then no bytes should be read/written, but
    // evnt should still be set when blkCount bytes can be read/written.
    bool dryOp : 1;
    enum irp_op op : 1;
} irp;

typedef struct user_irp {
    irp* obj;
    void* ubuffer;
} user_irp;

// desc can be nullptr if request->desc can be implied from vn.
OBOS_EXPORT obos_status VfsH_IRPSubmit(irp* request, const dev_desc* desc);
OBOS_EXPORT obos_status VfsH_IRPBytesToBlockCount(vnode* vn, size_t nBytes, size_t *out);
OBOS_EXPORT obos_status VfsH_IRPWait(irp* request);
OBOS_EXPORT obos_status VfsH_IRPSignal(irp* request, obos_status status);
OBOS_EXPORT void VfsH_IRPRef(irp* request);
OBOS_EXPORT void VfsH_IRPUnref(irp* request);
OBOS_EXPORT irp* VfsH_IRPAllocate();