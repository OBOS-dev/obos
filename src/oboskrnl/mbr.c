/*
 * oboskrnl/mbr.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <struct_packing.h>
#include <partition.h>
#include <mbr.h>

#include <vfs/fd.h>
#include <vfs/limits.h>

obos_status OBOS_IdentifyMBRPartitions(fd* desc, partition* partition_list, size_t* nPartitions)
{
    if (!desc || (!partition_list && !nPartitions))
        return OBOS_STATUS_INVALID_ARGUMENT;
    mbr_t mbr = {};
    size_t nRead = 0;
    Vfs_FdSeek(desc, 0, SEEK_END);
    size_t filesize = Vfs_FdTellOff(desc);
    Vfs_FdSeek(desc, 0, SEEK_SET);
    if (filesize < sizeof(mbr_t))
        return OBOS_STATUS_EOF;
    obos_status status = Vfs_FdRead(desc, &mbr, sizeof(mbr), &nRead);
    if (obos_is_error(status))
        return status;
    if (nRead != sizeof(mbr))
        return OBOS_STATUS_INTERNAL_ERROR;
    if (mbr.signature != MBR_BOOT_SIGNATURE)
        return OBOS_STATUS_INVALID_FILE;
    if (nPartitions)
        *nPartitions = 0;
    size_t blkSize = Vfs_FdGetBlkSz(desc);
    for (uint8_t i = 0; i < 4; i++)
    {
        mbr_partition* curr = &mbr.parts[i];
        if (!curr->nSectors)
            break;
        if (nPartitions)
            (*nPartitions)++;
        // Sanity check.
        if (((blkSize * curr->lba) + (curr->nSectors * curr->lba)) < filesize)
        {
            if (nPartitions)
                *nPartitions = 0;
            return OBOS_STATUS_INVALID_ARGUMENT;
        }
        if (!partition_list)
            continue;
        partition_list[i].off = curr->lba * blkSize;
        partition_list[i].drive = desc->vn;
        partition_list[i].format = PARTITION_FORMAT_MBR;
    }
    return OBOS_STATUS_SUCCESS;
}