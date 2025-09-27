/*
 * oboskrnl/power/event.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#if OBOS_ARCHITECTURE_HAS_ACPI
#include <locks/event.h>

#include <vfs/vnode.h>
#include <vfs/dirent.h>

#include <power/event.h>

#include <irq/dpc.h>

#include <uacpi/notify.h>
#include <uacpi/utilities.h>
#include <uacpi/event.h>

power_event_header OBOS_PowerEvents[OBOS_POWER_MAX_VALUE+1];

static void trigger_event_dpc(dpc* d, void* userdata)
{
    OBOS_UNUSED(d);
    power_event_header* event = userdata;
    OBOS_Debug("Triggering power event '%s'\n", event->name);
    Core_EventSet(&event->event, true);
}
static void trigger_event(power_event_header* event)
{
    event->trigger_count++;
    event->dpc.userdata = event;
    CoreH_InitializeDPC(&event->dpc, trigger_event_dpc, 0);
}

static uacpi_iteration_decision foreach_event(void* handler, uacpi_namespace_node* node, uint32_t unused2)
{
    OBOS_UNUSED(unused2);
    uacpi_install_notify_handler(node, handler, nullptr);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_status power_button_notify(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value)
{
    OBOS_UNUSED(context && node && value);
    trigger_event(&OBOS_PowerEvents[OBOS_POWER_BUTTON_EVENT]);
    return UACPI_STATUS_OK;
}
static uacpi_interrupt_ret power_button_fixed(uacpi_handle unused)
{
    OBOS_UNUSED (unused);
    trigger_event(&OBOS_PowerEvents[OBOS_POWER_MAX_VALUE]);
    OBOS_PowerEvents[OBOS_POWER_BUTTON_EVENT].activated = true;
    return UACPI_INTERRUPT_HANDLED;
}

static const char* event_names[OBOS_POWER_MAX_VALUE+1] = {
    "power_button"
};

void OBOS_InitializeACPIEvents()
{
    for (size_t i = 0; i < (OBOS_POWER_MAX_VALUE+1); i++)
    {
        OBOS_PowerEvents[i].event = EVENT_INITIALIZE(EVENT_NOTIFICATION);
        OBOS_PowerEvents[i].name = event_names[i];
        OBOS_PowerEvents[i].registered_to = Drv_AllocateVNode(nullptr, 0, 0, nullptr, VNODE_TYPE_CHR);
        OBOS_PowerEvents[i].registered_to->flags |= VFLAGS_EVENT_DEV;
        OBOS_PowerEvents[i].registered_to->un.evnt = &OBOS_PowerEvents[i].event;
        OBOS_PowerEvents[i].dent = Drv_RegisterVNode(OBOS_PowerEvents[i].registered_to, OBOS_PowerEvents[i].name);
    }
    if (uacpi_likely_success(uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button_fixed, UACPI_NULL)))
        OBOS_PowerEvents[OBOS_POWER_BUTTON_EVENT].activated = true;
    uacpi_find_devices("PNP0C0C", foreach_event, power_button_notify);
}
#else
void OBOS_InitializeACPIEvents()
{}
#endif