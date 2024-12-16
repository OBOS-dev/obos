/*
 * oboskrnl/power/init.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include "cmdline.h"
#include "driver_interface/pci.h"
#include "mm/bare_map.h"
#include <int.h>
#include <klog.h>

#include <power/init.h>
#include <power/suspend.h>

#include <uacpi/uacpi.h>
#include <uacpi/status.h>
#include <uacpi/context.h>

#define verify_status(st, in) \
if (st != UACPI_STATUS_OK)\
{\
    OBOS_Error("uACPI Failed in %s! Status code: %d, error message: %s\nAborting further uACPI initialization.", #in, st, uacpi_status_to_string(st));\
    return;\
}
#define verify_status_panic(st, in) \
if (st != UACPI_STATUS_OK)\
{\
    OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "uACPI Failed in %s! Status code: %d, error message: %s\nAborting further uACPI initialization.", #in, st, uacpi_status_to_string(st));\
    return;\
}

static void *tables_buf;
static size_t table_buf_size;
void OBOS_SetupEarlyTableAccess()
{
    table_buf_size = OBOS_GetOPTD("early-table-access-buf-size");
    if (!table_buf_size)
        table_buf_size = 4096;
    if (table_buf_size >= 16384)
    {
        OBOS_Warning("Early table access buffer size is greater than 16K. Truncating to 16K.\n");
        table_buf_size = 16384;
    }
    tables_buf = OBOS_BasicMMAllocatePages(table_buf_size, nullptr);
    uacpi_status st = uacpi_setup_early_table_access(tables_buf, table_buf_size);
    verify_status_panic(st, uacpi_setup_early_table_access);
}

void OBOS_InitializeUACPI()
{
    uacpi_context_set_log_level(UACPI_LOG_INFO);

    uacpi_status st = uacpi_initialize(0);
    verify_status(st, uacpi_initialize);

    st = uacpi_namespace_load();
    verify_status(st, uacpi_namespace_load);

    st = uacpi_namespace_initialize();
    verify_status(st, uacpi_namespace_initialize);

    OBOS_InitWakeGPEs();
}
