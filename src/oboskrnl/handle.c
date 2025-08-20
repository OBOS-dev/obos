/*
 * oboskrnl/handle.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <handle.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>
#include <scheduler/schedule.h>

#include <irq/irql.h>

#include <locks/mutex.h>

#include <vfs/alloc.h>
#include <vfs/fd.h>
#include <vfs/irp.h>
#include <vfs/mount.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <driver_interface/driverId.h>
#include <driver_interface/header.h>

#include <allocators/base.h>

OBOS_NO_UBSAN void OBOS_ExpandHandleTable(handle_table* table, size_t size)
{
    if (size <= table->size)
        return;
    const size_t oldSize = table->size;
    table->size = size;
    handle_desc* tmp_arr = ZeroAllocate(OBOS_KernelAllocator, table->size, sizeof(*table->arr), nullptr);
    if (table->arr)
        memcpy(tmp_arr, table->arr, sizeof(*table->arr)*oldSize);
    for (handle_desc* desc = table->head; desc; )
    {
        handle_desc *next = desc->un.next;
        if (next)
            desc->un.next = tmp_arr + (next - table->arr);
        desc = next;
    }
    if (table->head)
        table->head = tmp_arr + (table->head - table->arr);
    Free(OBOS_KernelAllocator, table->arr, sizeof(*table->arr)*oldSize);
    table->arr = tmp_arr;
    table->last_handle = oldSize;
}

void OBOS_InitializeHandleTable(handle_table* table)
{
    memzero(table, sizeof(*table));
    table->lock = MUTEX_INITIALIZE();
    OBOS_ExpandHandleTable(table, 64);
}
handle_table* OBOS_CurrentHandleTable()
{
    // can only access cpu local stuff at IRQL_DISPATCH
    irql oldIrql = Core_GetIrql() < IRQL_DISPATCH ? Core_RaiseIrql(IRQL_DISPATCH) : IRQL_INVALID;
    handle_table* table = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;
    if (oldIrql != IRQL_INVALID)
        Core_LowerIrql(oldIrql);
    return table;

}

void OBOS_LockHandleTable(handle_table* table)
{
    Core_MutexAcquire(&table->lock);
}
void OBOS_UnlockHandleTable(handle_table* table)
{
    Core_MutexRelease(&table->lock);
}
#define in_range(ra,rb,x) (((x) >= (ra)) && ((x) < (rb)))
handle_desc* OBOS_HandleLookup(handle_table* table, handle hnd, handle_type type, bool ignoreType, obos_status* status)
{
    OBOS_ASSERT(table);
    // First, validate the handle type.
    if (HANDLE_TYPE(hnd) >= LAST_VALID_HANDLE_TYPE)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    if (HANDLE_TYPE(hnd) != type && !ignoreType)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    hnd &= HANDLE_VALUE_MASK;
    if (hnd >= table->size)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr;
    }
    if (in_range(table->arr, table->arr+table->size, table->arr[hnd].un.next))
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr; // use-after-free
    }
    if (!table->arr[hnd].un.next)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return nullptr; // use-after-free; it is impossible for a handle in-use to be nullptr
    }
    *status = OBOS_STATUS_SUCCESS;
    if (!ignoreType)
        OBOS_ASSERT(table->arr[hnd].type == type);
    return &table->arr[hnd];
}
handle OBOS_HandleAllocate(handle_table* table, handle_type type, handle_desc** const desc)
{
    OBOS_ASSERT(table);
    OBOS_ASSERT(desc);
    handle hnd = 0;
    if (table->head)
    {
        hnd = table->head - table->arr;
        table->head = table->head->un.next;
    }
    else
    {
        if ((table->last_handle + 1) >= table->size)
            OBOS_ExpandHandleTable(table, OBOS_MAX(table->size + (table->size / 4), hnd));
        hnd = table->last_handle++;
    }
    *desc = &table->arr[hnd];
    memzero(&table->arr[hnd], sizeof(table->arr[hnd]));
    table->arr[hnd].type = type;
    hnd |= (type << HANDLE_TYPE_SHIFT);
    return hnd;
}
void OBOS_HandleFree(handle_table* table, handle_desc *curr)
{
    curr->type = HANDLE_TYPE_INVALID;
    curr->un.next = table->head;
    table->head = curr;
    // any use of this handle past here is a use-after-free
}

void unimpl_handle_clone(handle_desc *hnd, handle_desc *new)
{
    OBOS_UNUSED(new);
    OBOS_Warning("Cannot clone handle descriptor %p. Unimplemented.\n", hnd);
}

void fd_clone(handle_desc* hnd, handle_desc* new)
{
    new->un.fd = Vfs_Calloc(1, sizeof(fd));
    uint32_t oflags = 0;
    if (hnd->un.fd->flags & FD_FLAGS_READ)
        oflags |= FD_OFLAGS_READ;
    if (hnd->un.fd->flags & FD_FLAGS_WRITE)
        oflags |= FD_OFLAGS_WRITE;
    if (hnd->un.fd->flags & FD_FLAGS_UNCACHED)
        oflags |= FD_OFLAGS_UNCACHED;
    if (hnd->un.fd->flags & FD_FLAGS_NOEXEC)
        oflags |= FD_OFLAGS_NOEXEC;
    Vfs_FdOpenVnode(new->un.fd, hnd->un.fd->vn, oflags);
    new->un.fd->offset = hnd->un.fd->offset;
}

void(*OBOS_HandleCloneCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd, handle_desc *new) = {
    fd_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
    unimpl_handle_clone,
};

void fd_close(handle_desc* hnd)
{
    Vfs_FdClose(hnd->un.fd);
    Vfs_Free(hnd->un.fd);
}
void dirent_close(handle_desc* hnd)
{
    Free(OBOS_KernelAllocator, hnd->un.dirent, sizeof(struct dirent_handle));
}
void process_close(handle_desc* hnd)
{
    if (!(--hnd->un.process->refcount))
        Free(OBOS_NonPagedPoolAllocator, hnd->un.process, sizeof(process));
}
void irp_close(handle_desc* hnd)
{
    user_irp* req = hnd->un.irp;
    if (req->obj->evnt)
        CoreH_AbortWaitingThreads(WAITABLE_OBJECT(*req->obj->evnt));
    vnode* vn = req->obj->vn;
    mount* point = vn->mount_point ? vn->mount_point : vn->un.mounted;
    const driver_header* driver = vn->vtype == VNODE_TYPE_REG ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO || vn->vtype == VNODE_TYPE_SOCK)
        driver = &vn->un.device->driver->header;
    if (driver->ftable.unreference_device)
        driver->ftable.unreference_device(req->desc);
    if (req->obj->buff)
        Mm_VirtualMemoryFree(&Mm_KernelContext, req->obj->buff, req->buff_size);
    VfsH_IRPUnref(req->obj);
    Vfs_Free(req);
}

void(*OBOS_HandleCloseCallbacks[LAST_VALID_HANDLE_TYPE])(handle_desc *hnd) = {
    fd_close,
    nullptr,
    dirent_close,
    nullptr,
    process_close,
    nullptr, // TODO: Refcount vmm contexts.
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    irp_close,
};

static obos_status handle_close_unlocked(handle_table* current_table, handle hnd);

obos_status Sys_HandleClone(handle hnd, handle* unew)
{
    handle_table* const current_table = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;

    OBOS_LockHandleTable(current_table);
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* desc = OBOS_HandleLookup(current_table, hnd, 0, true, &status);
    if (obos_is_error(status))
    {
        OBOS_UnlockHandleTable(current_table);
        return status;
    }

    handle_type type = HANDLE_TYPE(hnd);
    if (!OBOS_HandleCloneCallbacks[type])
    {
        OBOS_UnlockHandleTable(current_table);
        return OBOS_STATUS_INVALID_OPERATION;
    }

    handle_desc* new_desc = nullptr;
    handle new = 0;
    memcpy_usr_to_k(&new, unew, sizeof(handle));
    OBOS_Debug("%s: *unew=%d\n", __func__, new);
    if (new == HANDLE_ANY)
    {
        new = OBOS_HandleAllocate(current_table, type, &new_desc);
        status = memcpy_k_to_usr(unew, &new, sizeof(new));
        if (obos_is_error(status))
        {
            OBOS_UnlockHandleTable(current_table);
            OBOS_HandleFree(current_table, new_desc);
            return status;
        }
    }
    else 
    {
        OBOS_ExpandHandleTable(current_table, new+1);
        new_desc = &current_table->arr[new];
        desc = &current_table->arr[hnd];
        handle_type type = new_desc->type;
        handle_close_unlocked(current_table, new | (type << 24));
	    current_table->head = new_desc->un.next;
    }

    void(*cb)(handle_desc *hnd, handle_desc *new) = OBOS_HandleCloneCallbacks[type];
    cb(desc, new_desc);
    new_desc->type = type;
    OBOS_UnlockHandleTable(current_table);

    return OBOS_STATUS_SUCCESS;
}
static obos_status handle_close_unlocked(handle_table* current_table, handle hnd)
{
    // Get the handle descriptor.
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* desc = OBOS_HandleLookup(current_table, hnd, 0, true, &status);
    if (obos_is_error(status))
    {
        OBOS_UnlockHandleTable(current_table);
        return status;
    }

    // Free the handle's underlying object as well as the handle itself.
    handle_type type = desc->type;
    void(*cb)(handle_desc *hnd) = OBOS_HandleCloseCallbacks[type];
    if (cb)
        cb(desc);
    OBOS_HandleFree(current_table, desc);
    return status;
}
obos_status Sys_HandleClose(handle hnd)
{
    handle_table* const current_table = &CoreS_GetCPULocalPtr()->currentThread->proc->handles;
    OBOS_LockHandleTable(current_table);

    obos_status status = handle_close_unlocked(current_table, hnd);

    OBOS_UnlockHandleTable(current_table);

    return status;
}
