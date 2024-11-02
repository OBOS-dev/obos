/*
 * oboskrnl/power/init.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

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
void OBOS_InitializeUACPI()
{
    uacpi_status st = uacpi_initialize(0);
    verify_status(st, uacpi_initialize);

    st = uacpi_namespace_load();
    verify_status(st, uacpi_namespace_load);

    st = uacpi_namespace_initialize();
    verify_status(st, uacpi_namespace_initialize);

    OBOS_InitWakeGPEs();
}
