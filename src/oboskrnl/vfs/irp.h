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
    // EVENT_NOTIFICATION.
    event evnt;
    union {
        void *buff;
        const void* cbuff;
    };
    size_t refs;
    uoff_t blkOffset;
    size_t blkCount;
    union {
        size_t nBlkRead;
        size_t nBlkWritten;
    };
    dev_desc desc;
    obos_status status;
    // If dryOp is true, then no bytes should be read/written, but
    // evnt should still be set when blkCount bytes can be read/written.
    bool dryOp : 1;
    enum irp_op op : 1;
} irp;
// desc can be nullptr if request->desc can be implied from vn.
obos_status VfsH_IRPSubmit(vnode* vn, irp* request, const dev_desc* desc);
obos_status VfsH_IRPBytesToBlockCount(vnode* vn, size_t nBytes, size_t *out);
obos_status VfsH_IRPWait(irp* request);
void VfsH_IRPRef(irp* request);
void VfsH_IRPUnref(irp* request);