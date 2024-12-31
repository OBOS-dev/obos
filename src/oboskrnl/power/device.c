/*
 * oboskrnl/power/device.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <klog.h>
#include <error.h>

// note: remove after done debugging pls
#include <uacpi/internal/namespace.h>
#include <uacpi/namespace.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <uacpi/event.h>
#include <uacpi/context.h>

#include <power/device.h>

obos_status OBOS_DeviceSetDState(uacpi_namespace_node* dev, d_state new_state, bool dry_run)
{
    if (new_state == DSTATE_INVALID || new_state > DSTATE_MAX || !dev)
        return OBOS_STATUS_INVALID_ARGUMENT;
    dry_run = !!dry_run;

    uacpi_status ustatus = UACPI_STATUS_OK;

    const char psn_path[] = { '_', 'P', 'S', '0' + new_state, '\0' };
    uacpi_namespace_node* psn = nullptr;
    uacpi_namespace_node_find(dev, psn_path, &psn);

    const char prn_path[] = { '_', 'P', 'R', '0' + new_state, '\0' };
    uacpi_namespace_node* prn = nullptr;
    uacpi_namespace_node_find(dev, prn_path, &prn);

    if (!psn && !prn)
        return OBOS_STATUS_NOT_FOUND;

    // Turn on all power resources needed.
    // TODO: Turn off all unneeded power resources.
    if (prn)
    {
        uacpi_object_array pkg = {};
        ustatus = uacpi_object_get_package(uacpi_namespace_node_get_object(prn), &pkg);
        if (uacpi_unlikely_error(ustatus))
            return OBOS_STATUS_INTERNAL_ERROR;
        for (size_t i = 0; i < pkg.count && !dry_run; i++)
        {
            uacpi_object* obj = pkg.objects[i];
            uacpi_namespace_node* pr = nullptr;
            ustatus = uacpi_object_resolve_as_aml_namepath(obj, nullptr, &pr);
            if (uacpi_unlikely_error(ustatus))
                continue; // an error, weird.
            ustatus = uacpi_eval_simple(pr, "_ON", nullptr);
            if (uacpi_unlikely_error(ustatus))
            {
                OBOS_Warning("Could not enable power resource. Status: %d. Continuing.\n", ustatus);
                continue;
            }
        }
    }

    ustatus = dry_run ? UACPI_STATUS_OK : uacpi_eval_simple(psn, nullptr, nullptr);
    return ustatus == UACPI_STATUS_OK ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
}

obos_status OBOS_DeviceHasDState(uacpi_namespace_node* dev, d_state state)
{
    if (state == DSTATE_INVALID || state > DSTATE_MAX || !dev)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (state == DSTATE_3COLD)
        state = DSTATE_3HOT;

    const char psn_path[] = { '_', 'P', 'S', '0' + state };
    uacpi_namespace_node* psn = nullptr;
    uacpi_namespace_node_find(dev, psn_path, &psn);

    const char prn_path[] = { '_', 'P', 'R', '0' + state };
    uacpi_namespace_node* prn = nullptr;
    uacpi_namespace_node_find(dev, prn_path, &prn);

    return !psn && !prn ? OBOS_STATUS_NOT_FOUND : OBOS_STATUS_SUCCESS;
}

static obos_status enable_wake_gpe(uacpi_namespace_node* dev, uacpi_object_array* pkg)
{
    OBOS_UNUSED(dev);
    uacpi_namespace_node* gpe_dev = nullptr;
    uint64_t gpe_idx = 0;

    if (uacpi_object_is(pkg->objects[0], UACPI_OBJECT_INTEGER))
    {
        uacpi_object_get_integer(pkg->objects[0], &gpe_idx);
        gpe_dev = nullptr;
    }
    else if (uacpi_object_is(pkg->objects[0], UACPI_OBJECT_PACKAGE))
    {
        uacpi_object_array pkg2 = {};
        uacpi_object_get_package(pkg->objects[0], &pkg2);
        uacpi_status ustatus = uacpi_object_resolve_as_aml_namepath(pkg2.objects[0], nullptr, &gpe_dev);
        if (uacpi_unlikely_error(ustatus))
            return OBOS_STATUS_INTERNAL_ERROR;
        uacpi_object_get_integer(pkg2.objects[1], &gpe_idx);
    }

    //OBOS_Debug("enabling gpe (node: %p) at index 0x%x\n", gpe_dev, gpe_idx);
    uacpi_status ustatus = uacpi_setup_gpe_for_wake(gpe_dev, gpe_idx, nullptr);
    //OBOS_Debug("uacpi_setup_gpe_for_wake returned %d\n", ustatus);
    if (uacpi_unlikely_error(ustatus))
        return OBOS_STATUS_INTERNAL_ERROR;
    return OBOS_STATUS_SUCCESS;
}
static void enable_pwr(uacpi_namespace_node* dev, uacpi_object_array *pkg, bool on)
{
    uacpi_namespace_node* pwr_resource = nullptr;
    for (size_t i = 2; i < pkg->count; i++)
    {
        uacpi_status ustatus = uacpi_object_resolve_as_aml_namepath(pkg->objects[i], dev, &pwr_resource);
        if (uacpi_unlikely_error(ustatus))
        {
            OBOS_Warning("%s: Could not resolve power resource for wake. Status: %d\nNote: Skipping...\n", __func__, ustatus);
            continue;
        }
        //OBOS_Debug("evaluating %04s.%s\n", pwr_resource->name.id, on ? "_ON" : "_OFF");
        ustatus = uacpi_eval_simple(pwr_resource, on ? "_ON" : "_OFF", nullptr);
        //OBOS_Debug("uacpi_eval returned %d\n", ustatus);
        if (uacpi_unlikely_error(ustatus))
        {
            if (ustatus != UACPI_STATUS_NOT_FOUND)
                OBOS_Warning("%s: Could not %s power resource for wake. Status: %d\nNote: Skipping...\n", __func__, on ? "enable" : "disable", ustatus);
            continue;
        }
    }
}
static obos_status dsw(uacpi_namespace_node* dev, bool enableWake, uacpi_sleep_state target_slp, d_state target_d_state)
{
    uacpi_object_array args = {};
    args.count = 3;
    uacpi_object* objs[3] = {};
    args.objects = objs;
    args.objects[0] = uacpi_object_create_integer(enableWake);
    args.objects[1] = uacpi_object_create_integer(target_slp);
    args.objects[2] = uacpi_object_create_integer(target_d_state == DSTATE_INVALID ? 0 : target_d_state);
    uacpi_status ustatus = uacpi_eval(dev, "_DSW", &args, nullptr);
    uacpi_object_unref(args.objects[0]);
    uacpi_object_unref(args.objects[1]);
    uacpi_object_unref(args.objects[2]);
    if (uacpi_likely(ustatus == UACPI_STATUS_NOT_FOUND))
        return OBOS_STATUS_NOT_FOUND;
    if (uacpi_unlikely_error(ustatus))
        return OBOS_STATUS_INTERNAL_ERROR;
    return OBOS_STATUS_SUCCESS;
}
static obos_status psw(uacpi_namespace_node* dev, bool enableWake)
{
    uacpi_object_array args = {};
    args.count = 1;
    uacpi_object* obj = uacpi_object_create_integer(enableWake);
    args.objects = &obj;

    uacpi_status ustatus = uacpi_eval(dev, "_PSW", &args, nullptr);
    uacpi_object_unref(args.objects[0]);
    if (uacpi_unlikely(ustatus == UACPI_STATUS_NOT_FOUND))
        return OBOS_STATUS_NOT_FOUND;
    if (uacpi_unlikely_error(ustatus))
        return OBOS_STATUS_INTERNAL_ERROR;
    return OBOS_STATUS_SUCCESS;
}
// Moves the device into the D state required, calls _DSW or _PSW, does necessary power resource stuffs, registers GPEs
obos_status OBOS_DeviceMakeWakeCapable(uacpi_namespace_node* dev, uacpi_sleep_state state, bool registerGPEOnly)
{
    if (!dev)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (state <= UACPI_SLEEP_STATE_S0 || state >= UACPI_SLEEP_STATE_S5)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;
    d_state new_dstate = OBOS_DeviceGetDStateForWake(dev, state, &status);
    if (obos_is_error(status))
        return status;

    uacpi_object* buf = {};
    if (uacpi_unlikely_error(uacpi_eval_simple_package(dev, "_PRW", &buf)))
        return OBOS_STATUS_INTERNAL_ERROR;

    uacpi_object_array pkg = {};
    uacpi_object_get_package(buf, &pkg);

    // If false, use _PSW on cleanup, otherwise use _DSW on cleanup.
    bool useDSW = false;

    if (registerGPEOnly)
        goto registerGPE;

    // Enable all power resources set by _PRW.
    enable_pwr(dev, &pkg, true);

    // Try evaluating _DSW
    status = dsw(dev, true, state, new_dstate);
    if (obos_is_error(status) && status != OBOS_STATUS_NOT_FOUND)
    {
        // Back out of any unfinished operation.
        //enable_pwr(dev, &pkg, false);
        return status;
    }
    if (status != OBOS_STATUS_NOT_FOUND)
    {
        useDSW = true;
        goto down; // We're done with that, go down.
    }
    // Evaluate _PSW.
    status = psw(dev, true);
    if (obos_is_error(status) && status != OBOS_STATUS_NOT_FOUND)
    {
        // Back out of any unfinished operation.
        //enable_pwr(dev, &pkg, false);
        return status;
    }
    useDSW = false;
    down:
    if (new_dstate != DSTATE_INVALID)
    {
        status = OBOS_DeviceSetDState(dev, new_dstate, false);
        if (obos_is_error(status))
        {
            // Back out of the unfinished operation.
            //enable_pwr(dev, &pkg, false);
            if (useDSW)
                dsw(dev, false, state, new_dstate);
            else
                psw(dev, false);
            return status;
        }
    }

registerGPE:
    status = enable_wake_gpe(dev, &pkg);
    if (obos_is_error(status))
    {
        // Back out of the unfinished operation.
        //enable_pwr(dev, &pkg, false);
        if (useDSW)
            dsw(dev, false, state, DSTATE_0);
        else
            psw(dev, false);
        OBOS_DeviceSetDState(dev, DSTATE_0, false);
        return status;
    }

    return OBOS_STATUS_SUCCESS;
}

static uint64_t eval_integer_node(uacpi_namespace_node* dev, const char* path)
{
    if (!path)
        return UINT64_MAX;
    uint64_t integer = 0;
    uacpi_status status = uacpi_eval_simple_integer(dev, path, &integer);
    return status == UACPI_STATUS_NOT_FOUND ? UINT64_MAX : integer;
}
d_state OBOS_DeviceGetDStateForWake(uacpi_namespace_node* dev, uacpi_sleep_state state, obos_status* status)
{
    if (!dev)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_ARGUMENT;
        return DSTATE_INVALID;
    }
    // Evaluate PRW, if it exists.
    //printf("%s:%d\n", __FILE__, __LINE__);
    uacpi_object* buf = {};
    uacpi_status ret = uacpi_eval_simple_package(dev, "_PRW", &buf);
    if (uacpi_unlikely_error(ret))
    {
        if (status)
            *status = ret == UACPI_STATUS_NOT_FOUND ? OBOS_STATUS_WAKE_INCAPABLE : OBOS_STATUS_INTERNAL_ERROR;
        return DSTATE_INVALID;
    }

    //printf("%s:%d\n", __FILE__, __LINE__);

    uacpi_object_array pkg = {};
    uacpi_object_get_package(buf, &pkg);
    if (uacpi_unlikely(pkg.count < 2))
    {
        if (status)
            *status = OBOS_STATUS_MISMATCH;
        return DSTATE_INVALID;
    }
    uint64_t prw_2 = 0;
    uacpi_object_get_integer(pkg.objects[1], &prw_2);
    if (prw_2 < state)
    {
        if (status)
            *status = OBOS_STATUS_WAKE_INCAPABLE;
        return DSTATE_INVALID;
    }

    //printf("%s:%d\n", __FILE__, __LINE__);

    // We have the deepest sleep state that this device can wake us in.
    // Now we need the D states from _SnD and _SnW

    char path_d[5] = { '_', 'S', '0' + state, 'D', '\0' };
    char path_w[5] = { '_', 'S', '0' + state, 'W', '\0' };

    uint64_t snd = eval_integer_node(dev, path_d);
    uint64_t snw = eval_integer_node(dev, path_w);

    d_state val = DSTATE_INVALID;

    if (snd == UINT64_MAX && snw == UINT64_MAX)
    {
        if (status)
            *status = OBOS_STATUS_SUCCESS;
        return DSTATE_INVALID;
    }
    if ((snd != UINT64_MAX && snw != UINT64_MAX) && snw >= snd)
    {
        if (status)
            *status = OBOS_STATUS_MISMATCH;
        return DSTATE_INVALID;
    }

    // printf("%s:%d\n", __FILE__, __LINE__);
    // printf("%04s.%s evaluated to %d\n", dev->name.text, path_d, snd);
    // printf("%04s.%s evaluated to %d\n", dev->name.text, path_w, snw);

    // Sort from deepest->shallowest
    d_state avaliableStates[DSTATE_MAX + 1] = {};
    size_t nStates = 0; // max is DSTATE_MAX + 1

    // TODO: Do we use <= or ==?
    if (snd <= DSTATE_2 && snw == UINT64_MAX)
    {
        nStates = 1;
        avaliableStates[0] = snd;
    }

    // printf("%s:%d\n", __FILE__, __LINE__);

    // TODO: Do we use <= or ==?
    if (snw <= DSTATE_2 && snd == UINT64_MAX)
    {
        nStates = 3;
        avaliableStates[0] = DSTATE_2;
        avaliableStates[1] = DSTATE_1;
        avaliableStates[2] = DSTATE_0;
    }

    // pritf("%s:%d\n", __FILE__, __LINE__);

    if (snd <= DSTATE_2 && snw == UINT64_MAX)
    {
        nStates = 1;
        avaliableStates[0] = DSTATE_2;
    }

    // printf("%s:%d\n", __FILE__, __LINE__);

    if (snd == DSTATE_2 && (snw >= DSTATE_3HOT && snw != UINT64_MAX))
    {
        nStates = 3;
        avaliableStates[0] = DSTATE_3COLD;
        avaliableStates[1] = DSTATE_3HOT;
        avaliableStates[2] = DSTATE_2;
    }

    // Choose the state.
    for (size_t i = 0; i < nStates; i++)
    {
        // printf("Attempting to use D state %d for wake\n", avaliableStates[i]);
        obos_status stat = OBOS_DeviceHasDState(dev, avaliableStates[i]);
        if (obos_is_error(stat) && stat != OBOS_STATUS_NOT_FOUND)
        {
            if (status) *status = stat;
            // printf("Query of D state %d for wake failed. Status: %d\n", avaliableStates[i], stat);
            return DSTATE_INVALID;
        }
        if (stat == OBOS_STATUS_NOT_FOUND)
            continue;
        // printf("D state %d is wake-capable and exists.\n", avaliableStates[i]);
        val = avaliableStates[i];
        break;
    }

    // printf("%s:%d\n", __FILE__, __LINE__);

    if (status)
        *status = OBOS_STATUS_SUCCESS;
    return val;
}

