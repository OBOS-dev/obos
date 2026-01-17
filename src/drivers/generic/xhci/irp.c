/*
 * drivers/generic/xhci/irp.c
 *
 * Copyright (c) 2026 Omar Berrow
 */

#include <int.h>
#include <klog.h>

#include <driver_interface/usb.h>

#include <allocators/base.h>

#include <mm/pmm.h>

#include <vfs/irp.h>

#include "xhci.h"

static void populate_trbs(irp* req, bool data_stage, xhci_normal_trb* trbs, size_t nRegions, struct physical_region *regions, xhci_endpoint_context* ep_ctx, bool in_endpoint)
{
    for (size_t i = 0; i < nRegions; i++)
    {
        XHCI_SET_TRB_TYPE(&trbs[i], (data_stage && !i) ? XHCI_TRB_DATA_STAGE : XHCI_TRB_NORMAL);

        if (regions[i].sz > UINT16_MAX)
        {
            req->status = OBOS_STATUS_INVALID_ARGUMENT;
            return;
        }

        trbs[i].length_td_size |= ((nRegions - i) & 0x3f) << 17;
        trbs[i].dbp = regions[i].phys;
        trbs[i].length_td_size |= regions[i].sz;
        req->usb_packet_length += regions[i].sz;
        if (regions[i].sz <= 8 && !in_endpoint && ep_ctx->max_packet_size >= 8)
        {
            // Immediate data
            trbs[i].dbp = *(uint64_t*)MmS_MapVirtFromPhys(regions[i].phys);
            trbs->flags_type |= BIT(6);
        }
        if (i != (nRegions - 1))
            trbs[i].flags_type |= BIT(4) /* chain bit */;
        else if (!data_stage)
            trbs[i].flags_type |= BIT(5) /* interrupt on completion bit */;
        else
            trbs[i].dir_resv = in_endpoint;
    }
    return;
}

static void irp_on_event_set(irp* req)
{
    struct xhci_inflight_trb_array* arr = req->drvData;
    struct xhci_inflight_trb* old_itrb = arr->itrbs[arr->index];
    obos_status status = OBOS_STATUS_IRP_RETRY;
    loop:
    if (arr->index == (arr->count-1))
        status = OBOS_STATUS_SUCCESS;
    struct xhci_inflight_trb* itrb = arr->itrbs[++arr->index];
    if (!itrb)
    {
        if (status == OBOS_STATUS_SUCCESS)
        {
            if (old_itrb)
            {
                Free(OBOS_KernelAllocator, old_itrb->resp, old_itrb->resp_length*4);
                Free(OBOS_KernelAllocator, old_itrb, sizeof(*old_itrb));
            }

            req->status = status;
            req->evnt = nullptr;
            return;
        }
        goto loop;
    }

    if (old_itrb)
    {
        req->status = (old_itrb->resp[2] >> 24) == 1 ? status : OBOS_STATUS_INTERNAL_ERROR;
        req->nBlkRead -= XHCI_GET_TRB_TRANSFER_LENGTH(old_itrb->resp);
    }
    if (arr->index < (arr->count-1))
        req->evnt = &itrb->evnt;
    else
        req->evnt = nullptr;

    if (old_itrb)
    {
        Free(OBOS_KernelAllocator, old_itrb->resp, old_itrb->resp_length*4);
        Free(OBOS_KernelAllocator, old_itrb, sizeof(*old_itrb));
    }
}

obos_status submit_irp(void* reqp)
{
    irp* req = reqp;

    if (req->blkCount != sizeof(usb_irp_payload))
        return OBOS_STATUS_INVALID_ARGUMENT;

    usb_irp_payload* payload = req->buff;

    usb_dev_desc* desc = (void*)req->desc;
    if (!desc->attached)
    {
        req->status = OBOS_STATUS_INTERNAL_ERROR;
        return OBOS_STATUS_SUCCESS;
    }

    OBOS_ENSURE(desc->info.slot);

    xhci_device* dev = desc->controller->handle;
    xhci_slot* slot = &dev->slots[desc->info.slot-1];

    bool in_endpoint = req->op == IRP_READ;
    const uint8_t target = payload->endpoint == 0 ? 0 : ((payload->endpoint+1)*2 + (req->op == IRP_WRITE));
    if (!slot->trb_ring[target].buffer.pg)
    {
        req->status = OBOS_STATUS_UNINITIALIZED;
        return OBOS_STATUS_SUCCESS;
    }
    xhci_endpoint_context* ep_ctx = get_xhci_endpoint_context(dev, xhci_get_device_context(dev, desc->info.slot), target+1);
    OBOS_ENSURE(MmS_UnmapVirtFromPhys(ep_ctx));
    uint8_t ep_type = ((ep_ctx->flags2 >> 3) & 0b111);

    if (payload->trb_type == USB_TRB_ISOCH && ep_type != 1 /* isoch out */ && ep_type != 5 /* isoch in */)
    {
        req->status = OBOS_STATUS_INVALID_OPERATION;
        return OBOS_STATUS_SUCCESS;
    }
    if ((payload->trb_type != USB_TRB_CONTROL) && ep_type == 4 /* control */)
    {
        req->status = OBOS_STATUS_INVALID_OPERATION;
        return OBOS_STATUS_SUCCESS;
    }
    
    switch (payload->trb_type) {
        case USB_TRB_NORMAL:
        {
            if (payload->payload.normal.nRegions > 63)
            {
                req->status = OBOS_STATUS_INVALID_ARGUMENT;
                return OBOS_STATUS_SUCCESS;
            }

            xhci_normal_trb *trbs = ZeroAllocate(OBOS_KernelAllocator, payload->payload.normal.nRegions, sizeof(xhci_normal_trb), nullptr);

            populate_trbs(req, false, trbs, payload->payload.normal.nRegions, payload->payload.normal.regions, ep_ctx, in_endpoint);
            if (req->status != OBOS_STATUS_SUCCESS)
            {
                Free(OBOS_KernelAllocator, trbs, payload->payload.normal.nRegions * sizeof(xhci_normal_trb));
                return OBOS_STATUS_SUCCESS;
            }
            
            xhci_direction dir = 0;
            if (req->op == IRP_READ) dir = XHCI_DIRECTION_IN;
            else if (req->op == IRP_WRITE) dir = XHCI_DIRECTION_OUT;

            struct xhci_inflight_trb_array* arr = 
                ZeroAllocate(OBOS_KernelAllocator,
                             1,
                             sizeof(*arr)+payload->payload.normal.nRegions*sizeof(struct xhci_inflight_trb*),
                             nullptr);
            arr->count = payload->payload.normal.nRegions;

            xhci_inflight_trb* first_itrb = nullptr;
            for (size_t i = 0; i < payload->payload.normal.nRegions; i++)
            {
                bool last_trb = ((i+1) < payload->payload.normal.nRegions);
                req->status = xhci_trb_enqueue_slot(dev, 
                                                    desc->info.slot-1,
                                                    payload->endpoint,
                                                    dir,
                                                    (uint32_t*)&trbs[i],
                                                    &arr->itrbs[i],
                                                    last_trb);
                if (obos_is_error(req->status))
                {
                    for (size_t j = 0; j < i; j++)
                    {
                        if (!arr->itrbs[j]) continue;
                        if (arr->itrbs[j]->resp)
                            Free(OBOS_KernelAllocator, arr->itrbs[j]->resp, 4*arr->itrbs[j]->resp_length);
                        Free(OBOS_KernelAllocator, arr->itrbs[j], sizeof(*arr->itrbs[j]));
                    }
                    Free(OBOS_KernelAllocator, arr, arr->count * sizeof(struct xhci_inflight_trb*) + sizeof(*arr));
                    break;
                }
                if (arr->itrbs[i] && !first_itrb)
                {
                    first_itrb = arr->itrbs[i];
                    arr->index = i;
                }
            }
            if (first_itrb)
                req->evnt = &first_itrb->evnt;
            req->drvData = arr;
            req->nBlkRead = req->usb_packet_length;
            req->on_event_set = irp_on_event_set;

            break;
        }
        case USB_TRB_CONTROL:
        {    
            if (payload->payload.setup.nRegions > 61)
            {
                req->status = OBOS_STATUS_INVALID_ARGUMENT;
                break;
            }
            size_t nDwords = 4*(2+payload->payload.setup.nRegions);
            uint32_t* trbs = ZeroAllocate(OBOS_KernelAllocator, nDwords, sizeof(uint32_t), nullptr);

            xhci_setup_stage_trb* setup_stage = (void*)trbs;
            XHCI_SET_TRB_TYPE(setup_stage, XHCI_TRB_SETUP_STAGE);
            setup_stage->bmRequestType = payload->payload.setup.bmRequestType;
            setup_stage->bRequest = payload->payload.setup.bRequest;
            setup_stage->wValue = payload->payload.setup.wValue;
            setup_stage->wIndex = payload->payload.setup.wIndex;
            setup_stage->wLength = payload->payload.setup.wLength;
            setup_stage->length = 8;
            if (payload->payload.setup.nRegions == 0)
                setup_stage->trt = 0x0;
            else if (req->op == IRP_WRITE)
                setup_stage->trt = 0x2;
            else
                setup_stage->trt = 0x3;
            setup_stage->flags_type |= BIT(6);

            xhci_status_stage_trb* status_stage_trb = (void*)&trbs[(1+payload->payload.setup.nRegions)*4];
            if (payload->payload.setup.nRegions != 0)
            {
                populate_trbs(req, true, (xhci_data_stage_trb*)(trbs+4), payload->payload.setup.nRegions, payload->payload.setup.regions, ep_ctx, in_endpoint);
                if (req->status != OBOS_STATUS_SUCCESS)
                {
                    Free(OBOS_KernelAllocator, trbs, nDwords * sizeof(uint32_t));
                    return OBOS_STATUS_SUCCESS;
                }
            }
            XHCI_SET_TRB_TYPE(status_stage_trb, XHCI_TRB_STATUS_STAGE);

            status_stage_trb->flags_type |= BIT(5);
            if (req->op == IRP_READ)
                status_stage_trb->dir_resv |= BIT(0);
            
            xhci_direction dir = 0;
            if (req->op == IRP_READ) dir = XHCI_DIRECTION_IN;
            else if (req->op == IRP_WRITE) dir = XHCI_DIRECTION_OUT;

            struct xhci_inflight_trb_array* arr = 
                ZeroAllocate(OBOS_KernelAllocator,
                             1,
                             sizeof(*arr)+(nDwords/4)*sizeof(struct xhci_inflight_trb*),
                             nullptr);
            arr->count = (nDwords/4);

            struct xhci_inflight_trb* first_itrb = nullptr;
            for (size_t i = 0; i < (nDwords/4); i++)
            {
                bool last_trb = (i == (nDwords/4)-1);
                req->status = xhci_trb_enqueue_slot(dev, 
                                                    desc->info.slot-1,
                                                    payload->endpoint,
                                                    dir,
                                                    &trbs[i*4],
                                                    &arr->itrbs[i],
                                                    last_trb);
                if (obos_is_error(req->status))
                {
                    for (size_t j = 0; j < i; j++)
                    {
                        if (!arr->itrbs[j]) continue;
                        if (arr->itrbs[j]->resp)
                            Free(OBOS_KernelAllocator, arr->itrbs[j]->resp, 4*arr->itrbs[j]->resp_length);
                        Free(OBOS_KernelAllocator, arr->itrbs[j], sizeof(*arr->itrbs[j]));
                    }
                    Free(OBOS_KernelAllocator, arr, arr->count * sizeof(struct xhci_inflight_trb*) + sizeof(*arr));
                    break;
                }
                if (arr->itrbs[i] && !first_itrb)
                {
                    first_itrb = arr->itrbs[i];
                    arr->index = i;
                }
            }
            if (first_itrb)
                req->evnt = &first_itrb->evnt;
            req->drvData = arr;
            req->nBlkRead = req->usb_packet_length;
            req->on_event_set = irp_on_event_set;

            break;
        }
        case USB_TRB_ISOCH:
        {
            req->status = OBOS_STATUS_UNIMPLEMENTED;
            break;
        }
        case USB_TRB_NOP:
        {
            xhci_nop_trb trb = {};
            XHCI_SET_TRB_TYPE(&trb, XHCI_TRB_NOP);
            trb.flags_type |= BIT(5);
            
            xhci_inflight_trb* itrb = nullptr;
            
            xhci_direction dir = 0;
            if (req->op == IRP_READ) dir = XHCI_DIRECTION_IN;
            else if (req->op == IRP_WRITE) dir = XHCI_DIRECTION_OUT;

            req->status = xhci_trb_enqueue_slot(dev, 
                                                desc->info.slot-1,
                                                payload->endpoint,
                                                dir,
                                                (uint32_t*)&trb,
                                                &itrb, true);
            req->evnt = &itrb->evnt;
            req->drvData = itrb;

            break;
        }
        case USB_TRB_CONFIGURE_ENDPOINT:
        {
            req->status = OBOS_STATUS_UNIMPLEMENTED;
            break;
        }
        default: return OBOS_STATUS_INVALID_ARGUMENT;
    }

    return OBOS_STATUS_SUCCESS;
}

obos_status finalize_irp(void* reqp)
{
    irp* req = reqp;
    struct xhci_inflight_trb_array* arr = req->drvData;
    if (!arr)
        return OBOS_STATUS_SUCCESS;
    usb_irp_payload* payload = req->buff;
    if (payload->trb_type == USB_TRB_NOP)
    {
        xhci_inflight_trb* itrb = req->drvData;
        req->status = (itrb->resp[2] >> 24) == 1 ? OBOS_STATUS_SUCCESS : OBOS_STATUS_INTERNAL_ERROR;
        req->nBlkRead -= XHCI_GET_TRB_TRANSFER_LENGTH(itrb->resp);
        Free(OBOS_KernelAllocator, itrb->resp, itrb->resp_length*4);
        Free(OBOS_KernelAllocator, itrb, sizeof(*itrb));
        return OBOS_STATUS_SUCCESS;
    }
    return Free(OBOS_KernelAllocator, arr, arr->count * sizeof(struct xhci_inflight_trb*) + sizeof(*arr));
}