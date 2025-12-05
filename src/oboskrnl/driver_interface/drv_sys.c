/*
 * oboskrnl/driver_interface/drv_sys.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <handle.h>
#include <memmanip.h>
#include <syscall.h>
#include <perm.h>

#include <driver_interface/drv_sys.h>
#include <driver_interface/loader.h>
#include <driver_interface/pnp.h>

#include <mm/alloc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <allocators/base.h>

handle Sys_LoadDriver(const void* file, size_t szFile, obos_status* ustatus)
{        
    obos_status status = OBOS_CapabilityCheck("drv/load");
    if (obos_is_error(status))
        return status;

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

obos_status Sys_StartDriver(handle driver, handle* mainThread)
{
    obos_status status = OBOS_CapabilityCheck("drv/start");
    if (obos_is_error(status))
        return status;

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), driver, HANDLE_TYPE_DRIVER_ID, false, &status);
    if (!drv)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    handle tmp = HANDLE_INVALID;
    status = memcpy_k_to_usr(mainThread, &tmp, sizeof(handle));
    if (obos_is_error(status) && mainThread != nullptr)
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
    obos_status status = OBOS_CapabilityCheck("drv/unload");
    if (obos_is_error(status))
        return status;
    
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), driver, HANDLE_TYPE_DRIVER_ID, false, &status);
    if (!drv)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    driver_id* id = drv->un.driver_id;

    // Otherwise, refcount is too high for the driver to be unloaded
    Sys_HandleClose(driver);

    return Drv_UnloadDriver(id);
}

obos_status Sys_PnpLoadDriversAt(handle dent, bool wait)
{
    obos_status status = OBOS_CapabilityCheck("drv/load-pnp");
    if (obos_is_error(status))
        return status;
    
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
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
handle Sys_FindDriverByName(const char* uname /* assumed to be 64-bytes at max */)
{
    obos_status status = OBOS_CapabilityCheck("drv/open-name");
    if (obos_is_error(status))
        return HANDLE_INVALID;

    char* name = nullptr;
    size_t sz_name = 0;
    status = OBOSH_ReadUserString(uname, nullptr, &sz_name);
    if (obos_is_error(status))
        return HANDLE_INVALID;
    if (sz_name >= 64)
        return HANDLE_INVALID;
    name = ZeroAllocate(OBOS_KernelAllocator, sz_name+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(uname, name, nullptr);

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
    Free(OBOS_KernelAllocator, name, sz_name);
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
    obos_status status = OBOS_CapabilityCheck("drv/enumerate");
    if (obos_is_error(status))
        return HANDLE_INVALID;
    
    driver_id* id = nullptr;

    if (curr != HANDLE_INVALID)
    {
        OBOS_LockHandleTable(OBOS_CurrentHandleTable());
        handle_desc* drv = OBOS_HandleLookup(OBOS_CurrentHandleTable(), curr, HANDLE_TYPE_DRIVER_ID, false, nullptr);
        if (!drv)
        {
            OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
            return HANDLE_INVALID;
        }
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        id = drv->un.driver_id;
        if (!id->node.next)
            return HANDLE_INVALID;
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
    obos_status status = OBOS_CapabilityCheck("drv/query-name");
    if (obos_is_error(status))
        return status;
    
    if (!sznamebuf)
        return OBOS_STATUS_INVALID_ARGUMENT; 

    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
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
