/*
 * uACPI/ec.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#if OBOS_ARCHITECTURE_HAS_ACPI

#include <int.h>
#include <klog.h>

#include <uacpi/event.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/io.h>
#include <uacpi/tables.h>
#include <uacpi/opregion.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

static uacpi_namespace_node* ec_node;
static uacpi_namespace_node* ec_gpe_node;
static uint16_t ec_gpe_idx;
static spinlock ec_lock;
static struct acpi_gas ec_control_register;
static struct acpi_gas ec_data_register;
static bool ec_initialized;

// Stolen from managarm.

#define EC_OBF (1 << 0)
#define EC_IBF (1 << 1)
#define EC_BURST (1 << 4)
#define EC_SCI_EVT (1 << 5)

#define RD_EC 0x80
#define WR_EC 0x81
#define BE_EC 0x82
#define BD_EC 0x83
#define QR_EC 0x84

#define BURST_ACK 0x90

static uint8_t ec_read_reg(struct acpi_gas* reg)
{
    uint64_t val = 0;
    // Wait until OBF goes to one.
    do {
        uacpi_gas_read(&ec_control_register, &val);
    } while (!(val & EC_OBF));
    // Read the register.
    uacpi_gas_read(reg, &val);
    return val;
}

static void ec_write_reg(struct acpi_gas* reg, uint8_t what)
{
    uint64_t val = 0;
    // Wait until IBF goes to zero.
    do {
        uacpi_gas_read(&ec_control_register, &val);
    } while (val & EC_IBF);

    // Write the register.
    uacpi_gas_write(reg, what);
}

static uint8_t ec_read(uint8_t offset)
{
    ec_write_reg(&ec_control_register, BD_EC);
    ec_write_reg(&ec_data_register, offset);
    return ec_read_reg(&ec_data_register);
}

static uacpi_status ec_write(uint8_t offset, uint8_t value)
{
    ec_write_reg(&ec_control_register, BD_EC);
    ec_write_reg(&ec_data_register, offset);
    ec_write_reg(&ec_data_register, value);
    return UACPI_STATUS_OK;
}

static void ec_burst_enable()
{
    ec_write_reg(&ec_control_register, BE_EC);
    if (ec_read_reg(&ec_data_register) != BURST_ACK)
        OBOS_Warning("ACPI: Burst not acknoledged by EC, ignoring.\n");
}

static void ec_burst_disable()
{
    ec_write_reg(&ec_control_register, BD_EC);
    uint64_t val = 0;
    do {
        uacpi_gas_read(&ec_control_register, &val);
    } while (val & EC_BURST);
}

static uacpi_status ec_read_uacpi(uacpi_region_rw_data* data)
{
    data->value = ec_read(data->offset);
    return UACPI_STATUS_OK;
}

static uacpi_status ec_write_uacpi(uacpi_region_rw_data* data)
{
    ec_write(data->offset, data->value);
    return UACPI_STATUS_OK;
}

#define IRQL_EC_BURST IRQL_MASKED

// MUST be called at (or greater than) IRQL_GPE.
static bool ec_query(uint8_t *idx)
{
    uint8_t status = ec_read_reg(&ec_control_register);
    if (~status & EC_SCI_EVT)
        return false;

    OBOS_ASSERT(Core_GetIrql() < IRQL_GPE);
    irql oldIrql = Core_RaiseIrql(IRQL_EC_BURST);

    ec_burst_enable();

    // Query the embedded controller.
    ec_write_reg(&ec_control_register, QR_EC);
    *idx = ec_read_reg(&ec_data_register);

    ec_burst_disable();

    Core_LowerIrql(oldIrql);

    // If index is zero, the event was spurious.
    return (bool)*idx;
}

static uacpi_status ecRegionCB(uacpi_region_op op, uacpi_handle data)
{
    switch (op) {
        case UACPI_REGION_OP_ATTACH:
        case UACPI_REGION_OP_DETACH:
            return UACPI_STATUS_OK;
        case UACPI_REGION_OP_READ:
            return ec_read_uacpi((uacpi_region_rw_data*)data);
        case UACPI_REGION_OP_WRITE:
            return ec_write_uacpi((uacpi_region_rw_data*)data);
        default:
            return UACPI_STATUS_OK;
    }
}

void onECQuery(uacpi_handle hnd)
{
    // Evaluate whatever the EC wants us to evaluate.
    uint8_t idx = (uint8_t)(uintptr_t)hnd;
    char method[] = { '_', 'Q', 0,0,0 };
    snprintf(method+2, 3, "%02x", idx);
    uacpi_eval_simple(ec_node, method, nullptr);
    uacpi_finish_handling_gpe(ec_gpe_node, ec_gpe_idx);
}

uacpi_interrupt_ret onECEvent(uacpi_handle udata, uacpi_namespace_node* gpe_dev, uint16_t gpe_idx)
{
    OBOS_UNUSED(udata);
    OBOS_UNUSED(gpe_dev);
    OBOS_UNUSED(gpe_idx);

    irql oldIrql = Core_SpinlockAcquire(&ec_lock);

    uint8_t idx = 0;
    if (!ec_query(&idx))
        return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;

    uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, onECQuery, (void*)(uintptr_t)idx);

    Core_SpinlockRelease(&ec_lock, oldIrql);
    return UACPI_INTERRUPT_HANDLED;
}

static void install_ec_handlers()
{
    uacpi_install_address_space_handler(ec_node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ecRegionCB, nullptr);

    // Evaluate _GPE.
    uacpi_object* obj = nullptr;
    // Apparently, GPE block devices don't actually exist, so ignore that possiblity.
    uacpi_status status = uacpi_eval_simple_typed(ec_node, "_GPE", UACPI_OBJECT_INTEGER_BIT/*|UACPI_OBJECT_PACKAGE_BIT*/, &obj);
    if (uacpi_unlikely_error(status))
        return;

    if (uacpi_object_get_type(obj) == UACPI_OBJECT_INTEGER)
    {
        uint64_t val = 0;
        uacpi_object_get_integer(obj, &val);
        ec_gpe_idx = val;
        ec_gpe_node = nullptr;
    } /*else if (uacpi_object_get_type(obj) == UACPI_OBJECT_PACKAGE)
    {
        uacpi_object_array* pkg = 0;
        uacpi_object_get_package(obj, &pkg);
        if (pkg->count < 2)
            return;
        uint64_t val = 0;
        uacpi_object_get_integer(pkg->objects[1], &val);
        ec_gpe_idx = val;
        uacpi_object_resolve_as_aml_namepath(pkg->objects[0], uacpi_namespace_root(), &ec_gpe_node);
    }*/

    uacpi_install_gpe_handler(ec_gpe_node, ec_gpe_idx, UACPI_GPE_TRIGGERING_EDGE, onECEvent, nullptr);
}

void OBOS_InitializeECFromECDT()
{
    OBOS_ASSERT(!ec_initialized);
    uacpi_table tbl = {};
    uacpi_status status = uacpi_table_find_by_signature("ECDT", &tbl);
    if (status != UACPI_STATUS_OK)
    {
        OBOS_Log("ACPI: No ECDT found, EC will be initialized after namespace initialization.\n");
        return;
    }

    struct acpi_ecdt* ecdt = tbl.ptr;

    ec_node = nullptr;
    uacpi_namespace_node_find(nullptr, ecdt->ec_id, &ec_node);
    if (!ec_node)
    {
        OBOS_Error("ACPI: ECDT found, but path \"%s\" is invalid.\n", ecdt->ec_id);
        return;
    }

    ec_control_register = ecdt->ec_control;
    ec_data_register = ecdt->ec_data;

    install_ec_handlers();

    ec_initialized = true;
    // wooo

    OBOS_Log("ACPI: Initialized EC from ECDT (pre-namespace init)\n");
}

static uacpi_iteration_decision ec_enumerate_resources(void *user, uacpi_resource *resource)
{
    uint8_t* current_index = user;
    struct acpi_gas current_gas = {};
    switch (resource->type) {
        case UACPI_RESOURCE_TYPE_IO:
            current_gas.address = resource->io.minimum;
            current_gas.register_bit_width = resource->io.length*8;
            (*current_index)++;
            break;
        case UACPI_RESOURCE_TYPE_FIXED_IO:
            current_gas.address = resource->fixed_io.address;
            current_gas.register_bit_width = resource->fixed_io.length*8;
            (*current_index)++;
            break;
        default:
            return UACPI_ITERATION_DECISION_CONTINUE;
    }
    current_gas.address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;
    switch (*current_index) {
        case 0:
            ec_data_register = current_gas;
            break;
        case 1:
            ec_control_register = current_gas;
            break;
        default:
            return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision ec_match(void* udata, uacpi_namespace_node* node, uint32_t unused)
{
    OBOS_UNUSED(udata);
    OBOS_UNUSED(unused);

    struct uacpi_resources *resources = nullptr;
    uacpi_status status = uacpi_get_current_resources(node, &resources);
    if (uacpi_unlikely_error(status))
        return UACPI_ITERATION_DECISION_BREAK;

    uint8_t current_index = 0;
    uacpi_for_each_resource(resources, ec_enumerate_resources, &current_index);
    uacpi_free_resources(resources);

    if (current_index < 2)
        return UACPI_ITERATION_DECISION_CONTINUE;

    ec_node = node;
    install_ec_handlers();
    ec_initialized = true;

    return UACPI_ITERATION_DECISION_BREAK;
}

void OBOS_InitializeECFromNamespace()
{
    if (ec_initialized)
        return; // This is called after namespace init unconditionally, so instead of crashing, fail gracefully.
    uacpi_find_devices("PNP0C09", ec_match, nullptr);
    if (!ec_initialized)
        OBOS_Log("ACPI: Machine has no EC\n");
    else
        OBOS_Log("ACPI: Initialized EC from namespace (post-namespace init)\n");

}

#endif
