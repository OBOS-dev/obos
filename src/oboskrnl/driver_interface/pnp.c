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

#include <utils/tree.h>

#include <utils/list.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/fd.h>
#include <vfs/vnode.h>

#if OBOS_ARCHITECTURE_HAS_ACPI
#include <uacpi/uacpi.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi_libc.h>
#endif

#include <scheduler/thread.h>

typedef struct pnp_device
{
    pci_hid pci_key; // the class code, subclass, etc.
    bool ignore_progif;
    bool pci : 1;
    bool acpi : 1;
    char acpi_key[8]; // acpi pnp id
    driver_header_list headers;
    RB_ENTRY(pnp_device) acpi_node;
    RB_ENTRY(pnp_device) pci_node;
} pnp_device;
typedef RB_HEAD(acpi_pnp_device_tree, pnp_device) acpi_pnp_device_tree;
typedef RB_HEAD(pci_pnp_device_tree, pnp_device) pci_pnp_device_tree;
static int pnp_pci_driver_cmp(pnp_device* a_, pnp_device* b_);
#if OBOS_ARCHITECTURE_HAS_ACPI
static int pnp_acpi_driver_compare(pnp_device* a, pnp_device* b);
RB_GENERATE_STATIC(acpi_pnp_device_tree, pnp_device, acpi_node, pnp_acpi_driver_compare);
#endif
RB_GENERATE_STATIC(pci_pnp_device_tree, pnp_device, pci_node, pnp_pci_driver_cmp);

static int pnp_pci_driver_cmp(pnp_device* a_, pnp_device* b_)
{
    // NOTE(oberrow): If this fails, gl and have fun.
    const pci_hid* a = &a_->pci_key;
    const pci_hid* b = &b_->pci_key;
    if (((int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode) != 0)
        return (int8_t)a->indiv.classCode - (int8_t)b->indiv.classCode;
    if (((int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass) != 0)
        return (int8_t)a->indiv.subClass - (int8_t)b->indiv.subClass;
    // if ((((int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf) != 0))
    //     return (int8_t)a->indiv.progIf - (int8_t)b->indiv.progIf;
    return 0;
}

#if OBOS_ARCHITECTURE_HAS_ACPI
static int pnp_acpi_driver_compare(pnp_device* a, pnp_device* b)
{
    return uacpi_strncmp(a->acpi_key, b->acpi_key, 8);
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

#if OBOS_ARCHITECTURE_HAS_ACPI
static void free_acpi_pnp_device(acpi_pnp_device_tree* map, pnp_device* dev)
{
    if (map)
        RB_REMOVE(acpi_pnp_device_tree, map, dev);
    for (driver_header_node* node = dev->headers.head; node; )
    {
        driver_header_node* nextNode = node->next;
        REMOVE_DRIVER_HEADER_NODE(dev->headers, node);
        free(node);
        node = nextNode;
    }
    // free(dev);
}
#endif
static void free_pci_pnp_device(pci_pnp_device_tree* map, pnp_device* dev)
{
    if (map)
        RB_REMOVE(pci_pnp_device_tree, map, dev);
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
static obos_status acpi_driver_helper(acpi_pnp_device_tree* acpi_drivers, driver_header* drv, char pnpId[8])
{
    pnp_device what = {
        .acpi_key = {
            pnpId[0], pnpId[1], pnpId[2], pnpId[3],
            pnpId[4], pnpId[5], pnpId[6], pnpId[7],  
        },
    };
    pnp_device *dev = RB_FIND(acpi_pnp_device_tree, acpi_drivers, &what);
    if (!dev)
    {
        dev = malloc(sizeof(pnp_device));
        *dev = what;
        RB_INSERT(acpi_pnp_device_tree, acpi_drivers, dev);
    }
    append_driver_to_pnp_device(dev, drv);
    return OBOS_STATUS_SUCCESS;
}
#endif

static obos_status pci_driver_helper(pci_pnp_device_tree* pci_drivers, driver_header* drv, pci_hid key)
{
    pnp_device what = {
        .pci_key = key,
    };
    pnp_device *dev = RB_FIND(pci_pnp_device_tree, pci_drivers, &what);
    if (!dev)
    {
        dev = memzero(malloc(sizeof(pnp_device)), sizeof(pnp_device));
        *dev = what;
        RB_INSERT(pci_pnp_device_tree, pci_drivers, dev);
    }
    append_driver_to_pnp_device(dev, drv);
    return OBOS_STATUS_SUCCESS;
}
struct callback_userdata
{
    pci_pnp_device_tree pci_drivers;
    acpi_pnp_device_tree acpi_drivers;
    driver_header_list* detected;
};
static pci_iteration_decision pci_driver_callback(void* udata_, pci_device *device)
{
    struct callback_userdata* udata = (struct callback_userdata*)udata_;

    pnp_device what = {
        .pci_key = device->hid,
    };
    pnp_device *dev = RB_FIND(pci_pnp_device_tree, &udata->pci_drivers, &what);
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
        
        free_pci_pnp_device(&udata->pci_drivers, dev);
    }
    return PCI_ITERATION_DECISION_CONTINUE;
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
    pnp_device *dev = RB_FIND(acpi_pnp_device_tree, &udata->acpi_drivers, &what);
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
        
        free_acpi_pnp_device(&udata->acpi_drivers, dev);
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
    acpi_pnp_device_tree acpi_drivers = RB_INITIALIZER(x);
#else
    bool acpi_drivers = 0;
#endif
#if OBOS_ARCHITECTURE_HAS_PCI
    pci_pnp_device_tree pci_drivers = RB_INITIALIZER(x);
#else
    bool pci_drivers = false;
#endif
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
                acpi_driver_helper(&acpi_drivers, drv, drv->acpiId.pnpIds[i]);
#endif
#if OBOS_ARCHITECTURE_HAS_PCI
        if ((drv->flags & DRIVER_HEADER_FLAGS_DETECT_VIA_PCI))
            pci_driver_helper(&pci_drivers, drv, drv->pciId);
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
    pnp_device* iter_pci = nullptr;
    pnp_device* next_pci = nullptr;
    RB_FOREACH_SAFE(iter_pci, pci_pnp_device_tree, &pci_drivers, next_pci)
        free_pci_pnp_device(&pci_drivers, iter_pci);
#endif
#if OBOS_ARCHITECTURE_HAS_ACPI
    // Enumerate ACPI
    uacpi_namespace_for_each_child_simple(
        uacpi_namespace_root(),
        acpi_enumerate_callback,
        &udata
    );
    // Free the acpi driver map.
    pnp_device* iter_acpi = nullptr;
    pnp_device* next_acpi = nullptr;
    RB_FOREACH_SAFE(iter_acpi, pci_pnp_device_tree, &pci_drivers, next_acpi)
        free_pci_pnp_device(&pci_drivers, iter_acpi);
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
    RB_ENTRY(driver_file) node;
};
static int driver_file_compare(struct driver_file* a, struct driver_file* b)
{
    return (a->hdr < b->hdr) ? -1 : ((a->hdr > b->hdr) ? 1 : 0);
}
typedef RB_HEAD(driver_file_tree, driver_file) driver_file_tree;
RB_GENERATE_STATIC(driver_file_tree, driver_file, node, driver_file_compare);
static void driver_file_free(void* ele)
{
    struct driver_file* drv = ele;
    if (drv->id)
        Drv_UnrefDriver(drv->id);
    Mm_VirtualMemoryFree(&Mm_KernelContext, drv->base, drv->file->vn->filesize);
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
    driver_file_tree drivers = RB_INITIALIZER(x);
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
        if (strnlen(hdr->driverName, 64))
            OBOS_Log("Found driver '%*s'\n", strnlen(hdr->driverName, 64), hdr->driverName);
        else
            OBOS_Log("Found a driver.\n", strnlen(hdr->driverName, 64), hdr->driverName);
        struct driver_file *drv_file = memzero(malloc(sizeof(struct driver_file)), sizeof(struct driver_file));
        *drv_file = (struct driver_file){ .hdr=hdr, .base=buf, .file=file };
        driver_header_node* node = malloc(sizeof(driver_header_node));
        // hashmap_set(drivers, &drv_file);
        RB_INSERT(driver_file_tree, &drivers, drv_file);
        node->data = hdr;
        APPEND_DRIVER_HEADER_NODE(what, node);

        ent = ent->d_next_child;
    }
    if (RB_EMPTY(&drivers))
        return OBOS_STATUS_SUCCESS;
    driver_header_list toLoad = {};
    obos_status status = Drv_PnpDetectDrivers(what, &toLoad);
    if (obos_is_success(status))
    {
        for (driver_header_node* node = toLoad.head; node; )
        {
            driver_header_node* next = node->next;
            driver_header* const curr = node->data;
            node = next;
            struct driver_file ele = { .hdr=curr };
            // struct driver_file* file = (struct driver_file*)hashmap_get(drivers, &ele);
            struct driver_file* file = RB_FIND(driver_file_tree, &drivers, &ele);
            Vfs_FdSeek(file->file, 0, SEEK_END);
            size_t filesize = Vfs_FdTellOff(file->file);
            Vfs_FdSeek(file->file, 0, SEEK_SET);
            obos_status loadStatus = OBOS_STATUS_SUCCESS;
            if (strnlen(file->hdr->driverName, 64))
                OBOS_Log("Loading '%*s'\n", strnlen(file->hdr->driverName, 64), file->hdr->driverName);
            else
                OBOS_Log("Loading a driver...\n");
            driver_id* drv = Drv_LoadDriver(file->base, filesize, &loadStatus);
            if (obos_is_error(loadStatus))
            {
                OBOS_Warning("Could not load '%*s'. Status: %d\n", strnlen(file->hdr->driverName, 64), file->hdr->driverName, loadStatus);
                continue;
            }
            drv->refCnt++;
            file->id = drv;
            loadStatus = Drv_StartDriver(drv, nullptr);
            if (obos_is_error(loadStatus) && loadStatus != OBOS_STATUS_NO_ENTRY_POINT)
            {
                OBOS_Warning("Could not start '%*s'. Status: %d\n", strnlen(file->hdr->driverName, 64), file->hdr->driverName, loadStatus);
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
                struct driver_file *iter, *next;
                // while (hashmap_iter(drivers, &i, &item))
                RB_FOREACH_SAFE(iter, driver_file_tree, &drivers, next)
                {
                    if (!iter->id)
                        continue;
                    if (!iter->id->main_thread)
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
        free(node);
        node = next;
    }
    struct driver_file *iter, *next;
    RB_FOREACH_SAFE(iter, driver_file_tree, &drivers, next)
    {
        if (!iter->id)
            continue;
        if (!iter->id->main_thread)
            continue;
        driver_file_free(iter);
        RB_REMOVE(driver_file_tree, &drivers, iter);
    }
    return status;
}

#if OBOS_ENABLE_UHDA
#include <uhda/uhda.h>

UhdaController** Drv_uHDAControllers;
pci_device_location* Drv_uHDAControllersLocations;
size_t Drv_uHDAControllerCount;

void OBOS_InitializeHDAAudioDev();

obos_status Drv_PnpLoad_uHDA()
{
    // pci_hid target_class = {};
    // target_class.indiv.classCode = UHDA_MATCHING_CLASS;
    // target_class.indiv.subClass = UHDA_MATCHING_SUBCLASS;

    for (uint8_t bus = 0; bus < Drv_PCIBusCount; bus++)
    {
        for (pci_device* dev = LIST_GET_HEAD(pci_device_list, &Drv_PCIBuses[bus].devices); dev; )
        {
            if (uhda_class_matches(dev->hid.indiv.classCode, dev->hid.indiv.subClass) || 
                uhda_device_matches(dev->hid.indiv.vendorId, dev->hid.indiv.deviceId))
            {
                OBOS_Log("%02x:%02x:%02x: uHDA device match!\n",
                    dev->location.bus, dev->location.slot, dev->location.function
                );
                UhdaController* controller = nullptr;
                if (uhda_init(dev, &controller) == UHDA_STATUS_SUCCESS)
                {
                    Drv_uHDAControllers = Reallocate(OBOS_KernelAllocator,
                                              Drv_uHDAControllers, 
                                              (Drv_uHDAControllerCount+1)*sizeof(*Drv_uHDAControllers), 
                                              Drv_uHDAControllerCount*sizeof(*Drv_uHDAControllers),
                                              nullptr);
                    
                    Drv_uHDAControllersLocations = Reallocate(OBOS_KernelAllocator,
                                              Drv_uHDAControllersLocations, 
                                              (Drv_uHDAControllerCount+1)*sizeof(*Drv_uHDAControllersLocations), 
                                              Drv_uHDAControllerCount*sizeof(*Drv_uHDAControllersLocations),
                                              nullptr);
                    Drv_uHDAControllers[Drv_uHDAControllerCount++] = controller;
                    Drv_uHDAControllersLocations[Drv_uHDAControllerCount - 1] = dev->location;
                }
            }

            dev = LIST_GET_NEXT(pci_device_list, &Drv_PCIBuses[bus].devices, dev);
        }
    }

    OBOS_InitializeHDAAudioDev();

    return OBOS_STATUS_SUCCESS;
}
#else
obos_status Drv_PnpLoad_uHDA()
{
    return OBOS_STATUS_UNIMPLEMENTED;
}
#endif