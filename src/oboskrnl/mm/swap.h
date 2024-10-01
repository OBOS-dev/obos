/*
 * oboskrnl/mm/swap.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <irq/irql.h>

#include <mm/page.h>
#include <mm/handler.h>

typedef struct swap_device
{
    // *id needs to be aligned to OBOS_PAGE_SIZE if !huge_page, otherwise it needs to be aligned to OBOS_HUGE_PAGE_SIZE
    obos_status(* swap_resv)(struct swap_device* dev, uintptr_t* id, bool huge_page);
    obos_status(* swap_free)(struct swap_device* dev, uintptr_t id);
    obos_status(*swap_write)(struct swap_device* dev, uintptr_t id, page* pg);
    obos_status(* swap_read)(struct swap_device* dev, uintptr_t id, page* pg);
    obos_status(*deinit_dev)(struct swap_device* dev);
    void* metadata;
} swap_dev;
extern swap_dev* Mm_SwapProvider;

obos_status Mm_SwapOut(page_info* page);
obos_status Mm_SwapIn(page_info* page, fault_type* type);

extern phys_page_list Mm_DirtyPageList;
extern phys_page_list Mm_StandbyPageList;
extern size_t Mm_DirtyPagesBytes;
extern size_t Mm_DirtyPagesBytesThreshold;
void Mm_MarkAsDirty(page_info* pg);
void Mm_MarkAsStandby(page_info* pg);
void Mm_InitializePageWriter();
// Wakes up the page writer to free up memory
// Set 'wait' to true to wait for the page writer to release
void Mm_WakePageWriter(bool wait);
irql Mm_TakeSwapLock();
void Mm_ReleaseSwapLock(irql oldIrql);

obos_status Mm_ChangeSwapProvider(swap_dev* to);