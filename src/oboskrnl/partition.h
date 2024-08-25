/*
 * oboskrnl/partition.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/limits.h>

#include <driver_interface/driverId.h>

typedef enum partition_format
{
    PARTITION_FORMAT_MBR,
    PARTITION_FORMAT_GPT,
    PARTITION_FORMAT_RAW,
} partition_format;
typedef struct partition
{
    struct dirent* ent;
    struct vnode* vn;
    struct vnode* drive;
    uoff_t off;
    size_t size;
    partition_format format;
    driver_id* fs_driver;
} partition;