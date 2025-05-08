/*
 * oboskrnl/power/init.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <cmdline.h>
#include <memmanip.h>

#include <driver_interface/pci.h>

#include <mm/bare_map.h>

#include <power/init.h>
#include <power/suspend.h>

#include <irq/irql.h>

#if OBOS_ARCHITECTURE_HAS_ACPI

#include <uacpi/uacpi.h>
#include <uacpi/status.h>
#include <uacpi/context.h>
#include <uacpi/event.h>
#include <uacpi/namespace.h>
#include <uacpi/notify.h>
#include <uacpi/types.h>
#include <uacpi/osi.h>

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
    uacpi_context_set_log_level(UACPI_LOG_ERROR);
    table_buf_size = OBOS_GetOPTD_Ex("early-table-access-buf-size", OBOS_PAGE_SIZE);
    if (table_buf_size >= 16384)
    {
        OBOS_Warning("Early table access buffer size is greater than 16K. Truncating to 16K.\n");
        table_buf_size = 16384;
    }
    tables_buf = OBOS_BasicMMAllocatePages(table_buf_size, nullptr);
    uacpi_status st = uacpi_setup_early_table_access(tables_buf, table_buf_size);
    verify_status_panic(st, uacpi_setup_early_table_access);
}

uacpi_status default_notify(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value)
{
    OBOS_UNUSED(context);
    const uacpi_char* path = uacpi_namespace_node_generate_absolute_path(node);
    OBOS_Debug("ignoring firmware Notify(%s, 0x%02x) request, no listener.\n", path, value);
    uacpi_kernel_free((void*)path, strlen(path));
    return UACPI_STATUS_OK;
}

void OBOS_InitializeUACPI()
{
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);

    uint64_t flags = 0;
    if (OBOS_GetOPTF("acpi-no-osi"))
        flags |= UACPI_FLAG_NO_OSI;
    if (OBOS_GetOPTF("acpi-bad-xsdt"))
        flags |= UACPI_FLAG_BAD_XSDT;

    uacpi_status st = uacpi_initialize(flags);
    verify_status_panic(st, uacpi_initialize);

    if (OBOS_GetLogLevel() <= LOG_LEVEL_LOG)
        uacpi_context_set_log_level(UACPI_LOG_INFO);

    OBOS_InitializeECFromECDT();

    st = uacpi_namespace_load();
    verify_status_panic(st, uacpi_namespace_load);

    st = uacpi_namespace_initialize();
    verify_status_panic(st, uacpi_namespace_initialize);

    OBOS_InitializeECFromNamespace();

    OBOS_InitWakeGPEs();
    OBOS_ECSetGPEs();

    uacpi_install_notify_handler(uacpi_namespace_root(), default_notify, nullptr);

    uacpi_finalize_gpe_initialization();

    Core_LowerIrql(oldIrql);
}

#else

void OBOS_SetupEarlyTableAccess() {}
void OBOS_InitializeUACPI() {}

void OBOS_InitializeECFromECDT() {}
void OBOS_InitializeECFromNamespace() {}
void OBOS_ECSetGPEs() {}

#endif
