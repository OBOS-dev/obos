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

static void ec_wait_for_bit(struct acpi_gas* reg, uint8_t mask, uint8_t desired_mask)
{
    // printf("waiting for mask 0x%02x in %s to become 0x%02x\n", mask, reg == &ec_control_register ? "control register" : "data register", desired_mask);
    uint64_t val = 0;
    do {
        uacpi_status status = uacpi_gas_read(reg, &val);
        if (uacpi_unlikely_error(status))
            OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not read from GAS. Status: %s\n", uacpi_status_to_string(status));
    } while((val & mask) != desired_mask);
    // printf("wait done\n");
}

static uint8_t ec_read_reg(struct acpi_gas* reg)
{
    uint64_t val = 0x00;
    // Wait until OBF goes to one.
    if (reg != &ec_control_register)
        ec_wait_for_bit(&ec_control_register, EC_OBF, EC_OBF);
    // Read the register.
    uacpi_status status = uacpi_gas_read(reg, &val);
    if (uacpi_unlikely_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not read from GAS. Status: %s\n", uacpi_status_to_string(status));
    return val;
}

static void ec_write_reg(struct acpi_gas* reg, uint8_t what)
{
    // Wait until IBF goes to zero.
    ec_wait_for_bit(&ec_control_register, EC_IBF, 0);
    // Write the register.
    uacpi_status status = uacpi_gas_write(reg, what);
    if (uacpi_unlikely_error(status))
        OBOS_Panic(OBOS_PANIC_DRIVER_FAILURE, "Could not read from GAS. Status: %s\n", uacpi_status_to_string(status));
}


static uint8_t ec_read(uint8_t offset)
{
    ec_write_reg(&ec_control_register, RD_EC);
    ec_write_reg(&ec_data_register, offset);
    return ec_read_reg(&ec_data_register);
}

static uacpi_status ec_write(uint8_t offset, uint8_t value)
{
    ec_write_reg(&ec_control_register, WR_EC);
    ec_write_reg(&ec_data_register, offset);
    ec_write_reg(&ec_data_register, value);
    return UACPI_STATUS_OK;
}

#define IRQL_EC_BURST IRQL_MASKED

static bool ec_burst_enable()
{
    ec_write_reg(&ec_control_register, BE_EC);
    uint8_t response = ec_read_reg(&ec_data_register);
    if (response != BURST_ACK)
    {
        OBOS_Warning("ACPI: Burst not acknoledged by EC, ignoring. Expected: 0x%02x, got 0x%02x\n", BURST_ACK, response);
        return false;
    }
    return true;
}

static void ec_burst_disable(bool ack)
{
    if (!ack)
        return;
    ec_write_reg(&ec_control_register, BD_EC);
    ec_wait_for_bit(&ec_control_register, EC_BURST, 0);
}

static uacpi_status ec_read_uacpi(uacpi_region_rw_data* data)
{
    irql oldIrql = Core_SpinlockAcquireExplicit(&ec_lock, IRQL_GPE, true);
    bool ack = ec_burst_enable();
    data->value = ec_read(data->offset);
    ec_burst_disable(ack);
    Core_SpinlockRelease(&ec_lock, oldIrql);
    return UACPI_STATUS_OK;
}

static uacpi_status ec_write_uacpi(uacpi_region_rw_data* data)
{
    irql oldIrql = Core_SpinlockAcquireExplicit(&ec_lock, IRQL_GPE, true);
    bool ack = ec_burst_enable();
    ec_write(data->offset, data->value);
    ec_burst_disable(ack);
    Core_SpinlockRelease(&ec_lock, oldIrql);
    return UACPI_STATUS_OK;
}

// MUST be called at (or greater than) IRQL_GPE.
static bool ec_query(uint8_t *idx)
{
    uint8_t status = ec_read_reg(&ec_control_register);
    if (~status & EC_SCI_EVT)
        return false;

    // OBOS_ASSERT(Core_GetIrql() < IRQL_GPE);
    // irql oldIrql = Core_RaiseIrql(IRQL_EC_BURST);

    bool ack = ec_burst_enable();

    // Query the embedded controller.
    ec_write_reg(&ec_control_register, QR_EC);
    *idx = ec_read_reg(&ec_data_register);

    ec_burst_disable(ack);

    // Core_LowerIrql(oldIrql);

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
    snprintf(method+2, 3, "%02X", idx);
    uacpi_eval_simple(ec_node, method, nullptr);
    uacpi_finish_handling_gpe(ec_gpe_node, ec_gpe_idx);
}

uacpi_interrupt_ret onECEvent(uacpi_handle udata, uacpi_namespace_node* gpe_dev, uint16_t gpe_idx)
{
    OBOS_UNUSED(udata);
    OBOS_UNUSED(gpe_dev);
    OBOS_UNUSED(gpe_idx);

    irql oldIrql = Core_SpinlockAcquireExplicit(&ec_lock, IRQL_GPE, true);

    uint8_t idx = 0;
    if (!ec_query(&idx))
    {
        Core_SpinlockRelease(&ec_lock, oldIrql);
        return UACPI_INTERRUPT_HANDLED | UACPI_GPE_REENABLE;
    }

    uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, onECQuery, (void*)(uintptr_t)idx);

    Core_SpinlockRelease(&ec_lock, oldIrql);
    return UACPI_INTERRUPT_HANDLED;
}

static void install_ec_handlers()
{
    uacpi_install_address_space_handler(uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ecRegionCB, nullptr);

    // Evaluate _GPE.
    uint64_t tmp = 0;
    // Apparently, GPE block devices don't actually exist, so ignore that possiblity.
    uacpi_status status = uacpi_eval_simple_integer(ec_node, "_GPE", &tmp);
    if (uacpi_unlikely_error(status))
        return;
    ec_gpe_idx = tmp & 0xffff;

    status = uacpi_install_gpe_handler(ec_gpe_node, ec_gpe_idx, UACPI_GPE_TRIGGERING_EDGE, onECEvent, nullptr);
    if (uacpi_unlikely_error(status))
        OBOS_Error("Could not install GPE %d. Status: %s\n", ec_gpe_idx, uacpi_status_to_string(status));
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
            break;
        case UACPI_RESOURCE_TYPE_FIXED_IO:
            current_gas.address = resource->fixed_io.address;
            current_gas.register_bit_width = resource->fixed_io.length*8;
            ;
        default:
            return UACPI_ITERATION_DECISION_CONTINUE;
    }
    current_gas.address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;
    switch (*current_index) {
        case 0:
            ec_data_register = current_gas;
            (*current_index)++;
            break;
        case 1:
            ec_control_register = current_gas;
            (*current_index)++;
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
        return UACPI_ITERATION_DECISION_CONTINUE;

    uint8_t current_index = 0;
    uacpi_for_each_resource(resources, ec_enumerate_resources, &current_index);
    uacpi_free_resources(resources);

    if (current_index < 2)
        return UACPI_ITERATION_DECISION_CONTINUE;

    ec_node = node;

    return UACPI_ITERATION_DECISION_BREAK;
}

void OBOS_InitializeECFromNamespace()
{
    if (ec_initialized)
        return; // This is called after namespace init unconditionally, so instead of crashing, fail gracefully.
    uacpi_find_devices("PNP0C09", ec_match, nullptr);
    if (ec_node)
    {
        install_ec_handlers();
        ec_initialized = true;
    }
    if (!ec_initialized)
        OBOS_Log("ACPI: Machine has no EC\n");
    else
        OBOS_Log("ACPI: Initialized EC from namespace (post-namespace init)\n");

}

void OBOS_ECSetGPEs()
{
    if (ec_initialized)
        uacpi_enable_gpe(ec_gpe_node, ec_gpe_idx);
}

void OBOS_ECSave()
{
    uacpi_uninstall_address_space_handler(uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER);
}

void OBOS_ECResume()
{
    uacpi_install_address_space_handler(uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, ecRegionCB, ec_node);
}

#endif
