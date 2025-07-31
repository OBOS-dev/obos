/*
 * oboskrnl/partition.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/limits.h>
#include <vfs/dirent.h>

// #include <driver_interface/driverId.h>

#include <utils/uuid.h>
#include <utils/list.h>
#include <utils/string.h>

typedef enum partition_format
{
    PARTITION_FORMAT_MBR,
    PARTITION_FORMAT_GPT,
    PARTITION_FORMAT_RAW,
} partition_format;
typedef struct partition
{
    dirent* ent;
    struct vnode* vn;
    struct vnode* drive;
    uoff_t off;
    size_t size;
    partition_format format;
    struct driver_id* fs_driver;
    uuid part_uuid; // invalid when format != GPT
    string part_name; // optional
    string partid;
    LIST_NODE(partition_list, struct partition) node;
} partition;
typedef LIST_HEAD(partition_list, partition) partition_list;
LIST_PROTOTYPE(partition_list, partition, node);
extern partition_list OBOS_Partitions;

void OBOS_PartProbeAllDrives(bool check_checksum);
obos_status OBOS_PartProbeDrive(struct dirent* ent, bool check_checksum);
