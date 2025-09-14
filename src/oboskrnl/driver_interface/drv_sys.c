/*
 * oboskrnl/driver_interface/drv_sys.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>

#include <driver_interface/drv_sys.h>
#include <driver_interface/loader.h>
#include <driver_interface/pnp.h>

#include <mm/alloc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

handle Sys_LoadDriver(const void* file, size_t szFile, obos_status* ustatus)
{
    if (Core_GetCurrentThread()->proc->currentUID != ROOT_UID)
    {
        if (ustatus) *ustatus = OBOS_STATUS_ACCESS_DENIED;
        return HANDLE_INVALID;
    }

    obos_status status = OBOS_STATUS_SUCCESS;
    if (!szFile)
    {
        status = OBOS_STATUS_INVALID_ARGUMENT;
        if (ustatus) memcpy_k_to_usr(ustatus, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }
    const void* buf = Mm_MapViewOfUserMemory(CoreS_GetCPULocalPtr()->currentContext, (void*)file, nullptr, szFile, OBOS_PROTECTION_READ_ONLY, true, &status);
    if (!buf)
    {
        if (ustatus) memcpy_k_to_usr(ustatus, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }
    driver_id* id = Drv_LoadDriver(buf, szFile, &status);
    Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)buf, szFile);
    if (!id)
    {
        if (ustatus) memcpy_k_to_usr(ustatus, &status, sizeof(obos_status));
        return HANDLE_INVALID;
    }

    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_DRIVER_ID, &desc);
    id->refCnt++;
    desc->un.driver_id = id;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return ret;
}

#define perm_check_impl(ret) do {\
    if (Core_GetCurrentThread()->proc->currentUID != ROOT_UID)\
        return (ret);\
} while(0)
#define perm_check() perm_check_impl(OBOS_STATUS_ACCESS_DENIED)
#define perm_check_hnd() perm_check_impl(HANDLE_INVALID)

obos_status Sys_StartDriver(handle driver, handle* mainThread)
{
    perm_check();

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), driver, HANDLE_TYPE_DRIVER_ID, false, &status);
    if (!drv)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    handle tmp = HANDLE_INVALID;
    status = memcpy_k_to_usr(mainThread, &tmp, sizeof(handle));
    if (obos_is_error(status))
        return status;

    thread* mainThreadPtr = nullptr;
    status = Drv_StartDriver(drv->un.driver_id, mainThread ? &mainThreadPtr : nullptr);
    if (mainThread && obos_is_success(status))
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* desc = nullptr;
        handle hnd = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_THREAD, &desc);
        desc->un.thread = mainThreadPtr;
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        status = memcpy_k_to_usr(mainThread, &hnd, sizeof(handle));
    }

    return status;
}

obos_status Sys_UnloadDriver(handle driver)
{
    perm_check();
    
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), driver, HANDLE_TYPE_DRIVER_ID, false, &status);
    if (!drv)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Drv_UnloadDriver(drv->un.driver_id);
}

obos_status Sys_PnpLoadDriversAt(handle dent, bool wait)
{
    perm_check();
    
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* dirent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), dent, HANDLE_TYPE_DIRENT, false, &status);
    if (!dirent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return Drv_PnpLoadDriversAt(dirent->un.dirent->parent, wait);
}

// Finds a loaded driver by its name, and returns a handle to it.
handle Sys_FindDriverByName(const char* name /* assumed to be 64-bytes at max */)
{
    perm_check_hnd();

    driver_id *id = nullptr;
    for (driver_node* curr = Drv_LoadedDrivers.head; curr; )
    {
        if (strncmp(name, curr->data->header.driverName, 64))
        {
            id = curr->data;
            break;
        }

        curr = curr->next;
    }
    if (!id)
        return HANDLE_INVALID;

    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_DRIVER_ID, &desc);
    id->refCnt++;
    desc->un.driver_id = id;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return ret;
}

// Returns the next driver in the list
// The handle 'curr' is not automatically closed.
// If 'curr' is HANDLE_INVALID, the first item in the list is returned.
handle Sys_EnumerateLoadedDrivers(handle curr)
{
    perm_check_hnd();
    
    driver_id* id = nullptr;

    if (curr != HANDLE_INVALID)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        obos_status status = OBOS_STATUS_SUCCESS;
        handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), curr, HANDLE_TYPE_DRIVER_ID, false, &status);
        if (!drv)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return status;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        id = drv->un.driver_id;
        id = id->node.next->data;
    }
    else
        id = Drv_LoadedDrivers.head->data;

    handle_desc* desc = nullptr;
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle ret = OBOS_HandleAllocate(OBOS_CurrentHandleTable(), HANDLE_TYPE_DRIVER_ID, &desc);
    id->refCnt++;
    desc->un.driver_id = id;
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return ret;
}

// Queries the name of the driver in 'drv'.
obos_status Sys_QueryDriverName(handle driver, char* namebuf, size_t *sznamebuf /* need not be over 64 */)
{
    perm_check();
    
    if (!sznamebuf)
        return OBOS_STATUS_INVALID_ARGUMENT;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), driver, HANDLE_TYPE_DRIVER_ID, false, &status);
    if (!drv)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    size_t strlen_name = strnlen(drv->un.driver_id->header.driverName, 64);

    size_t sznamebuf_deref = 0;
    status = memcpy_usr_to_k(&sznamebuf_deref, sznamebuf, sizeof(size_t));
    if (obos_is_error(status))
        return status;

    if (namebuf)
    {
        status = memcpy_k_to_usr(namebuf, drv->un.driver_id->header.driverName, OBOS_MIN(sznamebuf_deref, strlen_name));
        if (obos_is_error(status))
            return status;
    }

    status = memcpy_k_to_usr(sznamebuf, &strlen_name, sizeof(size_t));
    if (obos_is_error(status))
        return status;

    return OBOS_STATUS_SUCCESS;
}
