/*
 * drivers/generic/slowfat/fat_irp.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <vfs/irp.h>

#include <driver_interface/header.h>

#include "structs.h"
#include "alloc.h"

struct irp_clus_node {
    uint32_t cluster;
    size_t nClusters;
    size_t ioOffset;
    size_t ioLimit;
    struct irp_clus_node *next, *prev;
};
struct slowfat_irp_data {
    fat_cache* cache;
    uint8_t *iter;
    irp *disk_irp, *parent_irp;
    struct irp_clus_node *clus_head;
    struct irp_clus_node *clus_tail;
};

#define bytesPerCluster(cache) ((cache)->bpb->sectorsPerCluster*(cache)->blkSize)

iterate_decision cluster_list_populate_cb(uint32_t cluster, obos_status status, void* userdata)
{
    OBOS_UNUSED(status);

    uintptr_t* udata = userdata;
    struct slowfat_irp_data* const irp_data = (void*)udata[0];
    int64_t* const blkCount = (int64_t*)udata[1];
    fat_cache* cache = (void*)udata[2];
    uintptr_t const realBlkCount = udata[3];
    uintptr_t const blkOffset = udata[4];
    
    struct irp_clus_node *node = nullptr;
    if (irp_data->clus_tail && (irp_data->clus_tail->cluster + irp_data->clus_tail->nClusters) == cluster)
    {
        node = irp_data->clus_tail;
        node->nClusters++;
        goto down;
    }
    node = ZeroAllocate(FATAllocator, 1, sizeof(struct irp_clus_node), nullptr);
    node->cluster = cluster;
    node->nClusters++;

    node->ioLimit = bytesPerCluster(cache)*node->nClusters;
    if (!irp_data->clus_head)
    {
        node->ioOffset = blkOffset % bytesPerCluster(cache);
        node->ioLimit -= node->ioOffset;
    }
    
    if (!irp_data->clus_head)
        irp_data->clus_head = node;
    if (irp_data->clus_tail)
        irp_data->clus_tail->next = node;
    node->prev = irp_data->clus_tail;
    irp_data->clus_tail = node;

    down:
    (*blkCount) -= bytesPerCluster(cache);
    if ((*blkCount) <= 0)
    {
        node->ioLimit = (bytesPerCluster(cache)*(node->nClusters-1)) + (realBlkCount % bytesPerCluster(cache));
        return ITERATE_DECISION_STOP;
    }
    return ITERATE_DECISION_CONTINUE;
}

void read_irp_event_set_cb(irp* req)
{
    // Before destroying the IRP, check if we need to copy any data,
    // or if status == IRP_RETRY, or if the IRP failed (in which case, propagate the error).

    struct slowfat_irp_data* irp_data = req->drvData;
    OBOS_ENSURE(irp_data);

    req->status = OBOS_STATUS_IRP_RETRY;
    
    if (irp_data->disk_irp)
    {
        if (irp_data->disk_irp->on_event_set)
            irp_data->disk_irp->on_event_set(irp_data->disk_irp);
        if (irp_data->disk_irp->status == OBOS_STATUS_IRP_RETRY)
            return;
        if (obos_is_error(irp_data->disk_irp->status))
        {
            req->status = irp_data->disk_irp->status;
            return;
        }
    }
    
    fat_cache* cache = irp_data->cache;
    
    if (irp_data->disk_irp)
    {
        if (((irp_data->clus_head->ioLimit % bytesPerCluster(cache)) || irp_data->clus_head->ioOffset))
        {
            memcpy(irp_data->iter, irp_data->disk_irp->buff + irp_data->clus_head->ioOffset, irp_data->clus_head->ioLimit);
            Free(FATAllocator, irp_data->disk_irp->buff, irp_data->clus_head->nClusters * bytesPerCluster(cache));
        }
        irp_data->iter += irp_data->clus_head->ioLimit;
        req->nBlkRead += irp_data->clus_head->ioLimit;

        if (irp_data->clus_head->next)
            irp_data->clus_head->next->prev = irp_data->clus_head->prev;
        if (irp_data->clus_head->prev)
            irp_data->clus_head->prev->next = irp_data->clus_head->next;
        irp_data->clus_head = irp_data->clus_head->next;
        if (irp_data->clus_tail == irp_data->clus_head)
            irp_data->clus_tail = nullptr;
        if (!irp_data->clus_head)
        {
            // The read has finished.
            req->status = OBOS_STATUS_SUCCESS;
            return;
        }
    }
   
    if (!irp_data->disk_irp)
        irp_data->disk_irp = VfsH_IRPAllocate();
    else
    {
        memzero(irp_data->disk_irp, sizeof(irp));
        irp_data->disk_irp->refs = 1;
    }
    irp* new_irp = irp_data->disk_irp;
    /*
     * If the clus_head->ioLimit != nClusters*bytesPerCluster, then we should use a separate side buffer to read the entire cluster(s),
     * and then copy it into irp_data->iter, but only copy ioLimit.
     * If clus_head->ioOffset != 0, then we should also use a separate side buffer, but instead of copying ioLimit bytes from
     * tmpBuff+0, copy ioLimit bytes from tmpBuff + ioOffset
    */
    VfsH_IRPBytesToBlockCount(cache->vn, irp_data->clus_head->nClusters*bytesPerCluster(cache), &new_irp->blkCount);
    VfsH_IRPBytesToBlockCount(cache->vn, ClusterToSector(cache, irp_data->clus_head->cluster)*cache->blkSize, &new_irp->blkOffset);
    new_irp->op = IRP_READ;
    new_irp->vn = cache->vn;
    new_irp->dryOp = req->dryOp;
    if ((irp_data->clus_head->ioLimit % bytesPerCluster(cache)) || irp_data->clus_head->ioOffset)
    {
        size_t buffSz = irp_data->clus_head->nClusters * bytesPerCluster(cache);
        new_irp->buff = Allocate(FATAllocator, buffSz, nullptr);
    }
    else
        new_irp->buff = irp_data->iter;
    VfsH_IRPSubmit(new_irp, nullptr);
    req->evnt = new_irp->evnt;

    req->status = OBOS_STATUS_IRP_RETRY;
}
obos_status submit_irp(void* request)
{
    irp* req = request;
    if (!req)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (!req->desc || !req->buff)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    if (!req->blkCount)
    {
        req->evnt = nullptr;
        req->status = OBOS_STATUS_SUCCESS;
        return OBOS_STATUS_SUCCESS;
    }
    
    fat_dirent_cache* cache_entry = (fat_dirent_cache*)req->desc;
    fat_cache* cache = cache_entry->owner;
    if (cache_entry->data.attribs & DIRECTORY)
        return OBOS_STATUS_NOT_A_FILE;

    struct slowfat_irp_data* irp_data = ZeroAllocate(FATAllocator, 1, sizeof(struct slowfat_irp_data), nullptr);
    irp_data->parent_irp = req;
    irp_data->cache = cache;
    req->drvData = irp_data;
    // Form a cluster list, if reading.
    if (req->op == IRP_READ)
    {
        if (req->blkOffset >= cache_entry->data.filesize)
        {
            req->nBlkRead = 0;
            return OBOS_STATUS_SUCCESS;
        }
        if ((req->blkOffset + req->blkCount) >= cache_entry->data.filesize)
            req->blkCount = cache_entry->data.filesize - req->blkOffset;
        uint32_t cluster = cache_entry->data.first_cluster_low;
        if (cache->fatType == FAT32_VOLUME)
            cluster |= ((uint32_t)cache_entry->data.first_cluster_high << 16);
        cluster = ClusterSeek(cache, cluster, req->blkOffset/bytesPerCluster(cache));
        if (cluster == UINT32_MAX)
        {
            req->status = OBOS_STATUS_INVALID_ARGUMENT; // TODO: Handle properly.
            Free(FATAllocator, irp_data, sizeof(*irp_data));
            req->drvData = nullptr;
            return OBOS_STATUS_SUCCESS;
        }
        int64_t tmpBlkCount = req->blkCount;
        if (tmpBlkCount % bytesPerCluster(cache))
            tmpBlkCount += (bytesPerCluster(cache) - (tmpBlkCount % bytesPerCluster(cache)));
        uintptr_t udata[] = { (uintptr_t)irp_data, (uintptr_t)&tmpBlkCount, (uintptr_t)cache, req->blkCount, req->blkOffset };
        FollowClusterChain(cache, cluster, cluster_list_populate_cb, &udata);
        irp_data->iter = req->buff;
        req->on_event_set = read_irp_event_set_cb;
        read_irp_event_set_cb(req);
    }
    else
        return OBOS_STATUS_UNIMPLEMENTED;

    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK obos_status finalize_irp(void* request);