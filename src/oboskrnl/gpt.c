/*
 * oboskrnl/gpt.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>
#include <mbr.h>
#include <gpt.h>
#include <partition.h>
#include <struct_packing.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/pmm.h>

#include <allocators/base.h>

#include <vfs/fd.h>
#include <vfs/limits.h>
#include <vfs/vnode.h>

#include <utils/string.h>

static uint32_t crc32_bytes_from_previous(void *data, size_t sz, uint32_t previousChecksum);
static uint32_t crc32_bytes(void *data, size_t sz);
obos_status OBOS_IdentifyGPTPartitions(fd *desc, partition *partition_list, size_t *nPartitions, bool allow_checksum_fail) 
{
    if (!desc || (!partition_list && !nPartitions))
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t nRead = 0;
    size_t filesize = desc->vn->filesize;
    if (filesize < sizeof(mbr_t))
        return OBOS_STATUS_EOF;
    OBOS_ASSERT(OBOS_PAGE_SIZE > sizeof(mbr_t));
    mbr_t *protective_mbr = ZeroAllocate(OBOS_KernelAllocator, 1, OBOS_PAGE_SIZE, nullptr);
    obos_status status =
        Vfs_FdRead(desc, protective_mbr, sizeof(*protective_mbr), &nRead);
    if (obos_is_error(status))
        return status;
    if (nRead != sizeof(mbr_t))
        return OBOS_STATUS_INTERNAL_ERROR;
    if (protective_mbr->signature != MBR_BOOT_SIGNATURE)
        return OBOS_STATUS_INVALID_FILE;
    Free(OBOS_KernelAllocator, protective_mbr, OBOS_PAGE_SIZE);

    size_t blkSize = Vfs_FdGetBlkSz(desc);
    OBOS_ASSERT(__builtin_popcount(blkSize) == 1);
    uint8_t *buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, blkSize, 0, 0,
                                        nullptr, &status);
    if (obos_is_error(status))
        return status;
    gpt_header hdr = {};
    status = Vfs_FdRead(desc, buf, blkSize, &nRead);
    if (obos_is_error(status))
        return status;
    if (nRead != blkSize)
        return OBOS_STATUS_INTERNAL_ERROR;
    hdr = *(gpt_header *)buf;
    if (hdr.signature != GPT_SIGNATURE)
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
        return OBOS_STATUS_INVALID_FILE;
    }
    if (!allow_checksum_fail)
    {
        bool retried = false;
        retry:
        (void)0;
        uint32_t hdr_checksum = hdr.checksum;
        hdr.checksum = 0;
        uint32_t our_checksum = crc32_bytes(&hdr, sizeof(hdr));
        hdr.checksum = hdr_checksum;
        if (retried && our_checksum != hdr_checksum)
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
            return OBOS_STATUS_INVALID_FILE;
        }
        if (our_checksum != hdr_checksum)
        {
            retried = true;
            // bruh
            Vfs_FdSeek(desc, hdr.alt_lba * blkSize, SEEK_SET);
            status = Vfs_FdRead(desc, buf, blkSize, &nRead);
            if (obos_is_error(status))
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
                return status;
            }
            if (nRead != blkSize)
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
                return OBOS_STATUS_INTERNAL_ERROR;
            }
            hdr = *(gpt_header *)buf;
            if (hdr.signature != GPT_SIGNATURE)
            {
                Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
                return OBOS_STATUS_INVALID_FILE;
            }
            goto retry;
        }
    }
    const size_t nPartitionEntriesPerSector = blkSize / hdr.sizeof_partiton_entry;
    const size_t nSectorsForPartitionTable = hdr.part_entry_count / nPartitionEntriesPerSector + (hdr.part_entry_count % nPartitionEntriesPerSector != 0);
    const uint64_t partitionTableLBA = hdr.part_table_lba;
    Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize);
    buf = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, blkSize * nSectorsForPartitionTable, 
                                        0, VMA_FLAGS_NON_PAGED,
                                        nullptr, &status);
    Vfs_FdSeek(desc, partitionTableLBA*blkSize, SEEK_SET);
    Vfs_FdRead(desc, buf, blkSize*nSectorsForPartitionTable, nullptr);
    if (!allow_checksum_fail)
    {
        uint32_t entriesCRC32 = crc32_bytes(buf, blkSize * nSectorsForPartitionTable);
        if (entriesCRC32 != hdr.partition_entry_checksum)
        {
            Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize * nSectorsForPartitionTable);
            return OBOS_STATUS_INVALID_FILE;
        }
    }
    size_t nPartitionEntries = 0;
    uint8_t* iter = buf;
    for (size_t i = 0, id = 0; i < nSectorsForPartitionTable; i++)
    {
        
        gpt_partition_entry* const volatile table = (gpt_partition_entry*)iter;
        size_t nPartitionEntriesOnSector = 0;
        for (; iter < (buf + blkSize*nSectorsForPartitionTable); iter += hdr.sizeof_partiton_entry, nPartitionEntriesOnSector++)
            if (memcmp_b(iter, 0, sizeof(uuid)))
                break;
        nPartitionEntries += nPartitionEntriesOnSector;
        if (!partition_list)
        {
            iter += (blkSize - (uintptr_t)iter % blkSize);
            continue;
        }
        for (size_t partEntry = 0; partEntry < nPartitionEntriesOnSector; partEntry++, id++)
        {
            partition_list[id].vn = desc->vn;
            partition_list[id].off = table[partEntry].begin_lba * blkSize;
            partition_list[id].size = (table[partEntry].end_lba - table[partEntry].begin_lba) * blkSize;
            memcpy(partition_list[id].part_uuid, &table[partEntry].part_uuid, sizeof(uuid));
            OBOS_InitString(&partition_list[id].part_name, "");
            for (size_t j = 0; table[partEntry].part_name[j] && j < 36; j++)
            {
                char ch[2] = { table[partEntry].part_name[j]&0xff, 0 };
                OBOS_AppendStringC(&partition_list[id].part_name, ch);
            }
        }
        // iter = (uint8_t*)(((uintptr_t)iter + (blkSize - 1)) & ~(blkSize - 1));
        iter += (blkSize - (uintptr_t)iter % blkSize);
    }
    if (nPartitions)
        *nPartitions = nPartitionEntries;
    Mm_VirtualMemoryFree(&Mm_KernelContext, buf, blkSize * nSectorsForPartitionTable);
    return OBOS_STATUS_SUCCESS;
}
static bool initialized_crc32 = false;
static uint32_t crctab[256];

// For future reference, we cannot hardware-accelerate the crc32 algorithm as
// x86-64's crc32 uses a different polynomial than that of GPT.

static void crcInit() {
  uint32_t crc = 0;
  for (uint16_t i = 0; i < 256; ++i) {
    crc = i;
    for (uint8_t j = 0; j < 8; ++j) {
      uint32_t mask = -(crc & 1);
      crc = (crc >> 1) ^ (0xEDB88320 & mask);
    }
    crctab[i] = crc;
  }
}
static uint32_t crc(const char *data, size_t len, uint32_t result) {
  for (size_t i = 0; i < len; ++i)
    result = (result >> 8) ^ crctab[(result ^ data[i]) & 0xFF];
  return ~result;
}
static uint32_t crc32_bytes_from_previous(void *data, size_t sz,
                                   uint32_t previousChecksum) {
  if (!initialized_crc32) {
    crcInit();
    initialized_crc32 = true;
  }
  return crc((char *)data, sz, ~previousChecksum);
}
static uint32_t crc32_bytes(void *data, size_t sz)
{
  if (!initialized_crc32) {
    crcInit();
    initialized_crc32 = true;
  }
  return crc((char *)data, sz, ~0U);
}
