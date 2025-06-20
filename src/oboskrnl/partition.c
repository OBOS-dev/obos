/*
 * oboskrnl/partition.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <partition.h>
#include <mbr.h>
#include <gpt.h>

#include <vfs/limits.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/fd.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>

#include <driver_interface/driverId.h>

#include <utils/string.h>
#include <utils/uuid.h>
#include <utils/list.h>

#include <uacpi_libc.h>

LIST_GENERATE(partition_list, partition, node);
partition_list OBOS_Partitions;

void OBOS_PartProbeAllDrives(bool check_checksum)
{
    // for (volatile bool b = true; b; )
    //     ;
    dirent* directory = Vfs_DevRoot;
    for (dirent* ent = directory->d_children.head; ent; )
    {
        if (ent->vnode->vtype == VNODE_TYPE_BLK && !(ent->vnode->flags & VFLAGS_PARTITION))
        {
            OBOS_Log("Probing drive %*s for partitions...\n", OBOS_GetStringSize(&ent->name), OBOS_GetStringCPtr(&ent->name));
            obos_status status = OBOS_PartProbeDrive(ent, check_checksum);
            if (obos_is_error(status))
                OBOS_Error("Could not probe drive %s. Status: %d\n", OBOS_GetStringCPtr(&ent->name), status);
        }
        ent = ent->d_next_child;
    }
}
obos_status OBOS_PartProbeDrive(struct dirent* ent, bool check_checksum)
{
    partition *partitions = nullptr;
    size_t nPartitions = 0;
    fd drv = {};
    obos_status status = Vfs_FdOpenDirent(&drv, ent, FD_OFLAGS_UNCACHED|FD_OFLAGS_READ|FD_OFLAGS_WRITE);
    if (obos_is_error(status))
        return status;
    status = OBOS_IdentifyGPTPartitions(&drv, nullptr, &nPartitions, !check_checksum);
    Vfs_FdSeek(&drv, 0, SEEK_SET);
    if (obos_is_error(status) && status != OBOS_STATUS_INVALID_FILE)
    {
        Vfs_FdClose(&drv);
        return status;
    }
    if (status == OBOS_STATUS_INVALID_FILE)
    {
        // Fallback to MBR.
        status = OBOS_IdentifyMBRPartitions(&drv, NULL, &nPartitions);
        if (obos_is_error(status) && status != OBOS_STATUS_INVALID_FILE)
        {
            Vfs_FdClose(&drv);
            return status;
        }
        if (status == OBOS_STATUS_INVALID_FILE)
        {
            partitions = Vfs_Calloc(1, sizeof(partition));
            nPartitions = 1;
            partitions[0].drive = ent->vnode;
            partitions[0].off = 0;
            Vfs_FdSeek(&drv, 0, SEEK_END);
            partitions[0].size = Vfs_FdTellOff(&drv);
            Vfs_FdSeek(&drv, 0, SEEK_SET);
            partitions[0].format = PARTITION_FORMAT_RAW;
            goto done;
        }
        Vfs_FdSeek(&drv, 0, SEEK_SET);
        partitions = Vfs_Calloc(nPartitions, sizeof(partition));
        status = OBOS_IdentifyMBRPartitions(&drv, partitions, nullptr);
        if (obos_is_error(status))
        {
            Vfs_FdClose(&drv);
            return status;
        }
        for (size_t i = 0; i < nPartitions; i++)
            partitions[i].format = PARTITION_FORMAT_MBR;
        goto done;

    }
    if (!nPartitions)
    {
        Vfs_FdClose(&drv);
        return OBOS_STATUS_SUCCESS;
    }
    partitions = Vfs_Calloc(nPartitions, sizeof(partition));
    status = OBOS_IdentifyGPTPartitions(&drv, partitions, nullptr, check_checksum);
    if (obos_is_error(status))
    {
        Vfs_FdClose(&drv);
        return status;
    }
    for (size_t i = 0; i < nPartitions; i++)
        partitions[i].format = PARTITION_FORMAT_GPT;
    done:
    for (size_t i = 0; i < nPartitions; i++)
    {
        if (partitions[i].off > ent->vnode->filesize)
            continue;
        if ((partitions[i].off + partitions[i].size) > ent->vnode->filesize)
            partitions[i].size = ent->vnode->filesize-partitions[i].off;
        partitions[i].fs_driver = nullptr;
        // mount* const point = ent->vnode->mount_point ? ent->vnode->mount_point : ent->vnode->un.mounted;
        // driver_id* driver = ent->vnode->vtype == VNODE_TYPE_REG ? point->fs_driver->driver : nullptr;
        // if (ent->vnode->vtype == VNODE_TYPE_CHR || ent->vnode->vtype == VNODE_TYPE_BLK)
        //     driver = ent->vnode->un.device->driver;
        mount* const point = ent->vnode->mount_point ? ent->vnode->mount_point : ent->vnode->un.mounted;
        driver_id* driver = ent->vnode->vtype == VNODE_TYPE_REG ? point->fs_driver->driver : nullptr;
        if (ent->vnode->vtype == VNODE_TYPE_CHR || ent->vnode->vtype == VNODE_TYPE_BLK || ent->vnode->vtype == VNODE_TYPE_FIFO)
            driver = ent->vnode->un.device->driver;
        vnode* part_vnode = Drv_AllocateVNode(driver, ent->vnode->desc, partitions[i].size, nullptr, VNODE_TYPE_BLK);
        string part_name;
        OBOS_InitStringLen(&part_name, OBOS_GetStringCPtr(&ent->name), OBOS_GetStringSize(&ent->name));
        char num[21] = {};
        snprintf(num, 20, "%lu", i + 1);
        OBOS_AppendStringC(&part_name, num);
        part_vnode->flags |= VFLAGS_PARTITION;
        static const char* const part_formats[] = {
            "MBR",
            "GPT",
            "RAW",
        };
        OBOS_Log("Registering %s partition %s (\"%s\"). Partition ranges from 0x%016x-0x%016x\n", 
            part_formats[partitions[i].format],
            OBOS_GetStringCPtr(&part_name), 
            OBOS_GetStringCPtr(&partitions[i].part_name),
            partitions[i].off, partitions[i].off+partitions[i].size);
        if (partitions[i].format == PARTITION_FORMAT_GPT)
        {
            string uuid_str = {};
            OBOS_UUIDToString(&partitions[i].part_uuid, &uuid_str);
            OBOS_Log("Partition UUID: %s\n", OBOS_GetStringCPtr(&uuid_str));
            OBOS_FreeString(&uuid_str);
        }
        partitions[i].ent = Drv_RegisterVNode(part_vnode, OBOS_GetStringCPtr(&part_name));
        partitions[i].vn = part_vnode;
        partitions[i].partid = part_name;
        partitions[i].drive = ent->vnode;
        part_vnode->partitions = &partitions[i];
        part_vnode->nPartitions = 1; 
        for (driver_node* node = Drv_LoadedFsDrivers.head; node; )
        { 
            driver_header* hdr = &node->data->header;
            if (hdr->ftable.probe(part_vnode))
            {
                partitions[i].fs_driver = node->data;
                if (uacpi_strnlen(node->data->header.driverName, 32))
                    OBOS_Log("Partition recognized by '%*s'\n", uacpi_strnlen(node->data->header.driverName, 32), node->data->header.driverName);
                else
                    OBOS_Log("Partition recognized by a driver\n");
                break;
            }
            node = node->next;
        }
        LIST_APPEND(partition_list, &OBOS_Partitions, &partitions[i]);
    }
    ent->vnode->partitions = partitions;
    ent->vnode->nPartitions = nPartitions;
    VfsH_UnlockMountpoint(ent->vnode->mount_point);
    Vfs_FdClose(&drv);
    return OBOS_STATUS_SUCCESS;
}
