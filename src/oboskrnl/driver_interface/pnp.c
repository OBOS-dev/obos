/*
 * oboskrnl/driver_interface/pnp.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/header.h>
#include <driver_interface/loader.h>
#include <driver_interface/pnp.h>
#include <driver_interface/pci.h>

#include <allocators/base.h>

#include <utils/hashmap.h>

#include <uacpi_libc.h>

#include <utils/list.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/fd.h>
#include <vfs/vnode.h>

#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>

#include <scheduler/thread.h>

typedef struct pnp_device
{
    pci_hid pci_key; // the class code, subclass, etc.
    bool ignore_progif;
    char acpi_key[8]; // acpi pnp id
    driver_header_list headers;
} pnp_device;

static int pnp_pci_driver_cmp(const void* a_, const void* b_, void* udata)
{
    OBOS_UNUSED(udata);
    // NOTE(oberrow): If this fails, gl and have fun.
    const pci_hid* a = &((struct pnp_device*)a_)->pci_key;
    const pci_hid* b = &((struct pnp_device*)b_)->pci_key;
    if (((int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode) != 0)
        return (int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode;
    if (((int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass) != 0)
        return (int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass;
    // if ((((int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf) != 0))
    //     return (int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf;
    return 0;
}

static uint64_t pnp_pci_driver_hash(const void *item, uint64_t seed0, uint64_t seed1) 
{
    const struct pnp_device* drv = item;
    return hashmap_sip(&drv->pci_key.id, 4, seed0, seed1);
}
#if OBOS_ARCHITECTURE_HAS_ACPI
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
#endif

struct allocation_header {
    size_t size;
};

static void *malloc(size_t sz)
{
    struct allocation_header* hdr = ZeroAllocate(OBOS_KernelAllocator, 1, sz+sizeof(struct allocation_header), nullptr);
    hdr->size = sz+sizeof(struct allocation_header);
    return hdr + 1;
}

static void *realloc(void* oldblk, size_t sz)
{
    struct allocation_header* hdr = oldblk;
    hdr--;
    size_t oldSz = hdr->size;
    hdr->size += sz;
    return Reallocate(OBOS_KernelAllocator, hdr, sz, oldSz, nullptr);
}

static void free(void* blk)
{
    struct allocation_header* hdr = blk;
    hdr--;
    Free(OBOS_KernelAllocator, hdr, hdr->size);
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
    // '........' shit.
    driver_header_node* node = malloc(sizeof(driver_header_node));
    memzero(node, sizeof(*node));
    node->data = drv;
    APPEND_DRIVER_HEADER_NODE(dev->headers, node);
}
#if OBOS_ARCHITECTURE_HAS_ACPI
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
#endif

static obos_status pci_driver_helper(struct hashmap* pci_drivers, driver_header* drv, pci_hid key)
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
static pci_iteration_decision pci_driver_callback(void* udata_, pci_device *device)
{
    struct callback_userdata* udata = (struct callback_userdata*)udata_;

    pnp_device what = {
        .pci_key = device->hid,
    };
    pnp_device *dev = (pnp_device*)hashmap_get(udata->pci_drivers, &what);
    if (!dev)
        return PCI_ITERATION_DECISION_CONTINUE;

    // Add all of the drivers to the list.
    for (driver_header_node* node = dev->headers.head; node; )
    {
        driver_header_node* nextNode = node->next;
        driver_header* hdr = node->data;
        OBOS_ASSERT(hdr);
        bool shouldAdd = false;
        if (~hdr->flags & DRIVER_HEADER_PCI_IGNORE_PROG_IF)
        {
            if (hdr->pciId.indiv.progIf == device->hid.indiv.progIf)
                shouldAdd = true;
        }
        else
            shouldAdd = true;
        if (!shouldAdd)
            goto end;

        shouldAdd = false;
        if ((hdr->flags & DRIVER_HEADER_PCI_HAS_VENDOR_ID))
        {
            if (hdr->pciId.indiv.vendorId == device->hid.indiv.vendorId)
                shouldAdd = true;
        }
        else
            shouldAdd = true;
        if (!shouldAdd)
            goto end;

        shouldAdd = false;
        if ((hdr->flags & DRIVER_HEADER_PCI_HAS_DEVICE_ID))
        {
            if (hdr->pciId.indiv.deviceId == device->hid.indiv.deviceId)
                shouldAdd = true;
        }
        else
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
static uacpi_iteration_decision acpi_enumerate_callback(void *ctx, uacpi_namespace_node *node, uint32_t max_depth)
{
    OBOS_UNUSED(max_depth);

    struct callback_userdata* userdata = (struct callback_userdata*)ctx;

    uacpi_namespace_node_info *info;

    uacpi_status ret = uacpi_get_namespace_node_info(node, &info);
    if (uacpi_unlikely_error(ret))
        return UACPI_ITERATION_DECISION_CONTINUE;
    
    if (info->type != UACPI_OBJECT_DEVICE) 
    {
        uacpi_free_namespace_node_info(info);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) 
        probe_hid(&info->hid, userdata); // the result of this doesn't really matter.
    if (info->flags & UACPI_NS_NODE_INFO_HAS_CID) 
        for (size_t i = 0; i < info->cid.num_ids; i++)
            probe_hid(&info->cid.ids[i], userdata); // the result of this doesn't really matter.

    uacpi_free_namespace_node_info(info);
    return UACPI_ITERATION_DECISION_CONTINUE;
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
            pnp_acpi_driver_hash, pnp_acpi_driver_compare, nullptr, nullptr);
#else
    bool acpi_drivers = 0;
#endif
#if OBOS_ARCHITECTURE_HAS_PCI
    struct hashmap* pci_drivers = hashmap_new_with_allocator(
            malloc,
            realloc,
            free,
            sizeof(struct pnp_device),
            0, 0, 0,
            pnp_pci_driver_hash, pnp_pci_driver_cmp, nullptr, nullptr);
#else
    bool pci_drivers = false;
#endif
    if (!pci_drivers || !acpi_drivers)
        return OBOS_STATUS_INTERNAL_ERROR;
    // Divide the drivers into their respective hashmaps.
    for (driver_header_node* node = what.head; node; )
    {
        if (!node->data)
        {
            node = node->next;
            continue;
        }
        driver_header* drv = node->data;
        if (drv->flags & DRIVER_HEADER_PNP_IGNORE)
        {
            node = node->next;
            continue;
        }
#if OBOS_ARCHITECTURE_HAS_ACPI
        if ((drv->flags & DRIVER_HEADER_FLAGS_DETECT_VIA_ACPI))
            for (size_t i = 0; i < drv->acpiId.nPnpIds; i++) 
                acpi_driver_helper(acpi_drivers, drv, drv->acpiId.pnpIds[i]);
#endif
#if OBOS_ARCHITECTURE_HAS_PCI
        if ((drv->flags & DRIVER_HEADER_FLAGS_DETECT_VIA_PCI))
            pci_driver_helper(pci_drivers, drv, drv->pciId);
#endif

        node = node->next;
    }
    struct callback_userdata udata;
    OBOS_UNUSED(udata);
#if OBOS_ARCHITECTURE_HAS_ACPI
    udata.acpi_drivers = acpi_drivers;
#endif
    udata.detected = toLoad;
#if OBOS_ARCHITECTURE_HAS_PCI
    udata.pci_drivers = pci_drivers;
    // Enumerate the PCI bus.
    // DrvS_EnumeratePCI(pci_driver_callback, &udata);
    do {
        bool abort = false;
        for (size_t curr = 0; curr < Drv_PCIBusCount && !abort; curr++)
        {
            pci_bus* bus = &Drv_PCIBuses[curr];
            for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &bus->devices); dev && !abort; )
            {
                switch (pci_driver_callback(&udata, dev))
                {
                    case PCI_ITERATION_DECISION_ABORT:
                        abort = true;
                        break;
                    default:
                        break;
                }

                dev = LIST_GET_NEXT(pci_device_list, &bus->devices, dev);
            }
        }
    } while(0);
    // Free the pci driver map.
    free_map(pci_drivers);
#endif
#if OBOS_ARCHITECTURE_HAS_ACPI
    // Enumerate ACPI
    uacpi_namespace_for_each_child_simple(
        uacpi_namespace_root(),
        acpi_enumerate_callback,
        &udata
    );
    // Free the acpi driver map.
    free_map(acpi_drivers);
#endif
    // Return success.
    return OBOS_STATUS_SUCCESS;
}
struct driver_file
{
    driver_header* hdr;
    driver_id* id;
    void* base;
    fd* file;
};
static int driver_file_compare(const void* a_, const void* b_, void* udata)
{
    OBOS_UNUSED(udata);
    struct driver_file* a = (struct driver_file*)a_;
    struct driver_file* b = (struct driver_file*)b_;
    return (a->hdr < b->hdr) ? -1 : ((a->hdr > b->hdr) ? 1 : 0);
}
static uint64_t driver_file_hash(const void *item, uint64_t seed0, uint64_t seed1) 
{
    const struct driver_file* drv = item;
    return hashmap_sip(drv->hdr, sizeof(*drv->hdr), seed0, seed1);
}
static void driver_file_free(void* ele)
{
    struct driver_file* drv = ele;
    // if (drv->main)
    //     if (!(--drv->main->references) && drv->main->free)
    //         drv->main->free(drv->main);
    Vfs_FdSeek(drv->file, 0, SEEK_END);
    size_t filesize = Vfs_FdTellOff(drv->file);
    Vfs_FdSeek(drv->file, 0, SEEK_SET);
    Mm_VirtualMemoryFree(&Mm_KernelContext, drv->base, filesize);
    // drv->hdr is invalid.
    Vfs_FdClose(drv->file);
}
obos_status Drv_PnpLoadDriversAt(dirent* directory, bool wait)
{
    if (!directory)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (directory->vnode->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
    Vfs_PopulateDirectory(directory);
    struct hashmap* drivers = 
        hashmap_new_with_allocator(
            malloc,
            realloc,
            free,
            sizeof(struct driver_file),
            0, 0, 0,
            driver_file_hash, driver_file_compare, driver_file_free, nullptr);
    driver_header_list what = {};
    for (dirent* ent = directory->d_children.head; ent; )
    {
        fd* file = Vfs_Calloc(1, sizeof(fd));
        obos_status status = Vfs_FdOpenDirent(file, ent, FD_OFLAGS_READ);
        if (obos_is_error(status))
        {
            if (status != OBOS_STATUS_NOT_A_FILE)
                OBOS_Warning("Could not open file. Status: %d.\n", status);
            Vfs_Free(file);
            ent = ent->d_next_child;
            continue;
        }
        Vfs_FdSeek(file, 0, SEEK_END);
        size_t filesize = Vfs_FdTellOff(file);
        Vfs_FdSeek(file, 0, SEEK_SET);
        void* buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, filesize, 0, VMA_FLAGS_PRIVATE, file, &status);
        if (obos_is_error(status))
        {
            OBOS_Warning("Could not allocate file contents. Status: %d.\n", status);
            Vfs_FdClose(file);
            Vfs_Free(file);
            ent = ent->d_next_child;
            continue;
        }
        driver_header* hdr = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_header), nullptr);
        status = Drv_LoadDriverHeader(buf, filesize, hdr);
        if (obos_is_error(status))
        {
            if (status != OBOS_STATUS_INVALID_FILE)
                OBOS_Warning("Could not load driver header. Status: %d.\n", status);
            Mm_VirtualMemoryFree(&Mm_KernelContext, buf, filesize);
            Vfs_FdClose(file);
            Vfs_Free(file);
            ent = ent->d_next_child;
            continue;
        }
        if (uacpi_strnlen(hdr->driverName, 64))
            OBOS_Log("Found driver '%*s'\n", uacpi_strnlen(hdr->driverName, 64), hdr->driverName);
        else
            OBOS_Log("Found a driver.\n", uacpi_strnlen(hdr->driverName, 64), hdr->driverName);
        struct driver_file drv_file = { .hdr=hdr, .base=buf, .file=file };
        driver_header_node* node = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_header_node), nullptr);
        hashmap_set(drivers, &drv_file);
        node->data = hdr;
        APPEND_DRIVER_HEADER_NODE(what, node);

        ent = ent->d_next_child;
    }
    if (!hashmap_count(drivers))
        return OBOS_STATUS_SUCCESS;
    driver_header_list toLoad = {};
    obos_status status = Drv_PnpDetectDrivers(what, &toLoad);
    if (obos_is_success(status))
    {
        for (driver_header_node* node = toLoad.head; node; )
        {
            driver_header_node* next = node->next;
            driver_header* const curr = node->data;
            Free(OBOS_KernelAllocator, node, sizeof(*node));
            node = next;
            struct driver_file ele = { .hdr=curr };
            struct driver_file* file = (struct driver_file*)hashmap_get(drivers, &ele);
            Vfs_FdSeek(file->file, 0, SEEK_END);
            size_t filesize = Vfs_FdTellOff(file->file);
            Vfs_FdSeek(file->file, 0, SEEK_SET);
            obos_status loadStatus = OBOS_STATUS_SUCCESS;
            if (uacpi_strnlen(file->hdr->driverName, 64))
                OBOS_Log("Loading '%*s'\n", uacpi_strnlen(file->hdr->driverName, 64), file->hdr->driverName);
            else
                OBOS_Log("Loading a driver...\n", uacpi_strnlen(file->hdr->driverName, 64), file->hdr->driverName);
            driver_id* drv = Drv_LoadDriver(file->base, filesize, &loadStatus);
            if (obos_is_error(loadStatus))
            {
                OBOS_Warning("Could not load '%*s'. Status: %d\n", uacpi_strnlen(file->hdr->driverName, 64), file->hdr->driverName, loadStatus);
                continue;
            }
            file->id = drv;
            loadStatus = Drv_StartDriver(drv, nullptr);
            if (obos_is_error(loadStatus) && loadStatus != OBOS_STATUS_NO_ENTRY_POINT)
            {
                OBOS_Warning("Could not start '%*s'. Status: %d\n", uacpi_strnlen(file->hdr->driverName, 64), file->hdr->driverName, loadStatus);
                Drv_UnloadDriver(drv);
                continue;
            }
        }
        if (wait)
        {
            bool done = true;
            while (1)
            {
                done = true;
                size_t i = 0;
                void* item = nullptr;
                while (hashmap_iter(drivers, &i, &item))
                {
                    if (!item)
                        continue; // fnuy
                    struct driver_file* file = item;
                    if (!file->id)
                        continue;
                    if (!file->id->main_thread)
                        continue;
                    done = false;
                }
                if (done)
                    break;
            }
        }
    }
    for (driver_header_node* node = what.head; node; )
    {
        driver_header_node* next = node->next;
        Free(OBOS_KernelAllocator, node, sizeof(*node));
        node = next;
    }
    hashmap_clear(drivers, true);
    hashmap_free(drivers);
    return status;
}
