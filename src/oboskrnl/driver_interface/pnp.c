/*
 * oboskrnl/driver_interface/pnp.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "uacpi/status.h"
#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/pnp.h>
#include <driver_interface/pci.h>

#include <allocators/base.h>

#include <utils/hashmap.h>

#include <uacpi_libc.h>

#include <utils/list.h>

#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>

typedef struct pnp_device
{
    pci_device pci_key; // the class code, subclass, etc.
    char acpi_key[8]; // acpi pnp id
    driver_header_list headers;
} pnp_device;

static int pnp_pci_driver_cmp(const void* a_, const void* b_, void* udata)
{
    OBOS_UNUSED(udata);
    // NOTE(oberrow): If this fails, gl and have fun.
    const pci_device* a = &((struct pnp_device*)a_)->pci_key;
    const pci_device* b = &((struct pnp_device*)b_)->pci_key;
    if (((int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode) != 0)
        return (int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode;
    if (((int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass) != 0)
        return (int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass;
    if (((int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf) != 0)
        return (int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf;
    return 0;
}
static uint64_t pnp_pci_driver_hash(const void *item, uint64_t seed0, uint64_t seed1) 
{
    const struct pnp_device* drv = item;
    return hashmap_sip(&drv->pci_key.id, sizeof(drv->pci_key.id), seed0, seed1);
}
static int pnp_acpi_driver_compare(const void* a_, const void* b_, void* udata)
{
    OBOS_UNUSED(udata);
    struct pnp_device* a = (struct pnp_device*)a_;
    struct pnp_device* b = (struct pnp_device*)b_;
    return uacpi_strncmp(a->acpi_key, b->acpi_key, 8);
}
static uint64_t pnp_acpi_driver_hash(const void *item, uint64_t seed0, uint64_t seed1) 
{
    const struct pnp_device* drv = item;
    return hashmap_sip(&drv->acpi_key, sizeof(drv->acpi_key), seed0, seed1);
}

static void *malloc(size_t sz)
{
    return OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sz, nullptr);
}
static void *realloc(void* oldblk, size_t sz)
{
    return OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, oldblk, sz, nullptr);
}
static void free(void* blk)
{
    size_t sz = 0;
    OBOS_KernelAllocator->QueryBlockSize(OBOS_KernelAllocator, blk, &sz);
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, blk, sz);
}

static void free_pnp_device(struct hashmap* map, pnp_device* dev)
{
    if (map)
        hashmap_delete(map, dev);
    for (driver_header_node* node = dev->headers.head; node; )
    {
        driver_header_node* nextNode = node->next;
        REMOVE_DRIVER_HEADER_NODE(dev->headers, node);
        free(node);
        node = nextNode;
    }
    // free(dev);
}
static void append_driver_to_pnp_device(pnp_device* dev, driver_header* drv)
{
    OBOS_ASSERT(dev);
    // NOTE(oberrow): I hate myself for not adding a kmalloc or something eariler
    // Goddamn is this more convinient
    // The time it took to write this comment is less than the time it takes to write all the
    // 'OBOS_KernelAllocator->........' shit.
    driver_header_node* node = malloc(sizeof(driver_header_node));
    memzero(node, sizeof(*node));
    node->data = drv;
    APPEND_DRIVER_HEADER_NODE(dev->headers, node);
}
static obos_status acpi_driver_helper(struct hashmap* acpi_drivers, driver_header* drv, char pnpId[8])
{
    pnp_device what = {
        .acpi_key = {
            pnpId[0], pnpId[1], pnpId[2], pnpId[3],
            pnpId[4], pnpId[5], pnpId[6], pnpId[7],  
        },
    };
    pnp_device *dev = (pnp_device*)hashmap_get(acpi_drivers, &what);
    if (!dev)
    {
        hashmap_set(acpi_drivers, &what);
        if (hashmap_oom(acpi_drivers))
            return OBOS_STATUS_NOT_ENOUGH_MEMORY;
        dev = (pnp_device*)hashmap_get(acpi_drivers, &what);
    }
    append_driver_to_pnp_device(dev, drv);
    return OBOS_STATUS_SUCCESS;
}

static obos_status pci_driver_helper(struct hashmap* pci_drivers, driver_header* drv, pci_device key)
{
    pnp_device what = {
        .pci_key = key,
    };
    pnp_device *dev = (pnp_device*)hashmap_get(pci_drivers, &what);
    if (!dev)
    {
        hashmap_set(pci_drivers, &what);
        if (hashmap_oom(pci_drivers))
            return OBOS_STATUS_NOT_ENOUGH_MEMORY;
        dev = (pnp_device*)hashmap_get(pci_drivers, &what);
    }
    append_driver_to_pnp_device(dev, drv);
    return OBOS_STATUS_SUCCESS;
}
struct callback_userdata
{
    struct hashmap* pci_drivers;
    struct hashmap* acpi_drivers;
    driver_header_list* detected;
};
static pci_iteration_decision pci_driver_callback(void* udata_, pci_device_node device)
{
    struct callback_userdata* udata = (struct callback_userdata*)udata_;
    pnp_device what = {
        .pci_key = device.device,
    };
    pnp_device *dev = hashmap_get(udata->pci_drivers, &what);
    if (!dev)
        return PCI_ITERATION_DECISION_CONTINUE;
    // Add all of the drivers to the list.
    for (driver_header_node* node = dev->headers.head; node; )
    {
        driver_header_node* nextNode = node->next;
        driver_header* hdr = node->data;
        OBOS_ASSERT(hdr);
        bool shouldAdd = false;
        if ((hdr->flags & DRIVER_HEADER_PCI_HAS_VENDOR_ID))
            if (hdr->pciId.indiv.vendorId == device.device.indiv.vendorId)
                shouldAdd = true;
        if (!shouldAdd)
            goto end;
        shouldAdd = false;
        if ((hdr->flags & DRIVER_HEADER_PCI_HAS_DEVICE_ID))
            if (hdr->pciId.indiv.deviceId == device.device.indiv.deviceId)
                shouldAdd = true;
        if (!shouldAdd)
            goto end;
        driver_header_node* newNode = malloc(sizeof(driver_header_node));
        newNode->data = hdr;
        APPEND_DRIVER_HEADER_NODE(*udata->detected, newNode);
        REMOVE_DRIVER_HEADER_NODE(dev->headers, node);

        end:
        node = nextNode;
    }
    if (!dev->headers.nNodes)
    {
        // free the device.
        
        free_pnp_device(udata->pci_drivers, dev);
    }
    return PCI_ITERATION_DECISION_CONTINUE;
}
void free_map(struct hashmap* map)
{
    size_t iter = 0;
    void* dev = nullptr;
    while (hashmap_iter(map, &iter, &dev))
        free_pnp_device(nullptr, dev);
    hashmap_free(map);
}
#if OBOS_ARCHITECTURE_HAS_ACPI
static obos_status probe_hid(const uacpi_id_string *hid, struct callback_userdata* udata)
{
    OBOS_ASSERT((hid->size - 1) <= 8);
    if ((hid->size - 1) > 8)
        return OBOS_STATUS_INVALID_ARGUMENT;
    char* pnpId = hid->value;
    pnp_device what = {
        .acpi_key = {
            pnpId[0], pnpId[1], pnpId[2], pnpId[3],
            pnpId[4], pnpId[5], pnpId[6], pnpId[7],  
        }
    };
    pnp_device *dev = (pnp_device*)hashmap_get(udata->acpi_drivers, &what);
    if (!dev)
        return OBOS_STATUS_NOT_FOUND;
    // Add all of the drivers to the list.
    for (driver_header_node* node = dev->headers.head; node; )
    {
        driver_header_node* nextNode = node->next;
        driver_header* hdr = node->data;
        OBOS_ASSERT(hdr);
        bool shouldAdd = true;
        for (driver_header_node* n = udata->detected->head; n; )
        {
            if (n->data == hdr)
            {
                shouldAdd = false;
                break;
            }

            n = n->next;
        }
        if (!shouldAdd)
            goto end;
        driver_header_node* newNode = malloc(sizeof(driver_header_node));
        newNode->data = hdr;
        APPEND_DRIVER_HEADER_NODE(*udata->detected, newNode);
        REMOVE_DRIVER_HEADER_NODE(dev->headers, node);

        end:
        node = nextNode;
    }
    if (!dev->headers.nNodes)
    {
        // free the device.
        
        free_pnp_device(udata->acpi_drivers, dev);
    }
    return OBOS_STATUS_SUCCESS;
}
static uacpi_ns_iteration_decision acpi_enumerate_callback(void *ctx, uacpi_namespace_node *node)
{
    struct callback_userdata* userdata = (struct callback_userdata*)ctx;

    uacpi_namespace_node_info *info;

    uacpi_status ret = uacpi_get_namespace_node_info(node, &info);
    if (uacpi_unlikely_error(ret))
        return UACPI_NS_ITERATION_DECISION_CONTINUE;
    
    if (info->type != UACPI_OBJECT_DEVICE) 
    {
        uacpi_free_namespace_node_info(info);
        return UACPI_NS_ITERATION_DECISION_CONTINUE;
    }

    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) 
        probe_hid(&info->hid, userdata); // the result of this doesn't really matter.
    if (info->flags & UACPI_NS_NODE_INFO_HAS_CID) 
        for (size_t i = 0; i < info->cid.num_ids; i++)
            probe_hid(&info->cid.ids[i], userdata); // the result of this doesn't really matter.

    uacpi_free_namespace_node_info(info);
    return UACPI_NS_ITERATION_DECISION_CONTINUE;
}
#endif
obos_status Drv_PnpDetectDrivers(driver_header_list what, driver_header_list *toLoad)
{
    if (!toLoad)
        return OBOS_STATUS_INVALID_ARGUMENT;
#if OBOS_ARCHITECTURE_HAS_ACPI
    struct hashmap* acpi_drivers = 
        hashmap_new_with_allocator(
            malloc,
            realloc,
            free,
            sizeof(struct pnp_device),
            0, 0, 0,
            pnp_acpi_driver_hash, pnp_acpi_driver_compare, free, nullptr);
#else
    bool acpi_drivers = 0;
#endif
    struct hashmap* pci_drivers = DrvS_EnumeratePCI ?
        hashmap_new_with_allocator(
            malloc,
            realloc,
            free,
            sizeof(struct pnp_device),
            0, 0, 0,
            pnp_pci_driver_hash, pnp_pci_driver_cmp, free, nullptr) : nullptr;
    if (!pci_drivers || !acpi_drivers)
        return OBOS_STATUS_INTERNAL_ERROR;
    // Divide the drivers into their respective hashmaps.
    for (driver_header_node* node = what.head; node; )
    {
        if (!node->data)
            continue;
        driver_header* drv = node->data;
#if OBOS_ARCHITECTURE_HAS_ACPI
        if ((drv->flags & DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI))
            for (size_t i = 0; i < drv->acpiId.nPnpIds; i++) 
                acpi_driver_helper(acpi_drivers, drv, drv->acpiId.pnpIds[i]);
#endif
        if ((drv->flags & DRIVER_HEADER_FLAGS_DETECT_VIA_PCI) && DrvS_EnumeratePCI)
            pci_driver_helper(pci_drivers, drv, drv->pciId);

        node = node->next;
    }
    struct callback_userdata udata;
#if OBOS_ARCHITECTURE_HAS_ACPI
    udata.acpi_drivers = acpi_drivers;
#endif
    udata.pci_drivers = pci_drivers;
    udata.detected = toLoad;
    if (DrvS_EnumeratePCI) 
    {
        // Enumerate the PCI bus.
        DrvS_EnumeratePCI(pci_driver_callback, &udata);
        // Free the pci driver map.
        free_map(pci_drivers);
    }
#if OBOS_ARCHITECTURE_HAS_ACPI
    // Enumerate ACPI
    uacpi_namespace_for_each_node_depth_first(
        uacpi_namespace_root(),
        acpi_enumerate_callback,
        &udata
    );
    // Free the acpi driver map.
    free_map(acpi_drivers);
#endif
    // Return success;
    return OBOS_STATUS_SUCCESS;
}