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
    obos_status(* swap_free)(struct swap_device* dev, uintptr_t id, bool huge_page);
    obos_status(*swap_write)(struct swap_device* dev, uintptr_t id, page* pg);
    obos_status(* swap_read)(struct swap_device* dev, uintptr_t id, page* pg);
    obos_status(*deinit_dev)(struct swap_device* dev);
    void* metadata;
    size_t refs;
    bool awaiting_deinit;
    // Not to be initialized by the swap provider.
    // This simply frees the swap_dev object.
    void(*free_obj)(struct swap_device* dev);
} swap_dev;
extern swap_dev* Mm_SwapProvider;

obos_status Mm_SwapOut(uintptr_t virt, page_range* rng);
obos_status Mm_SwapIn(page_info* page, fault_type* type);

enum {
    PAGE_WRITER_SYNC_FILE = BIT(0),
    PAGE_WRITER_SYNC_ANON = BIT(1),
    PAGE_WRITER_SYNC_ALL = PAGE_WRITER_SYNC_FILE|PAGE_WRITER_SYNC_ANON,
};

extern uint32_t Mm_PageWriterOperation;

extern phys_page_list Mm_DirtyPageList;
extern phys_page_list Mm_StandbyPageList;
extern size_t Mm_DirtyPagesBytes;
extern size_t Mm_DirtyPagesBytesThreshold;
OBOS_EXPORT extern size_t Mm_CachedBytes;
OBOS_EXPORT void Mm_MarkAsDirty(page_info* pg);
OBOS_EXPORT void Mm_MarkAsStandby(page_info* pg);
OBOS_EXPORT void Mm_MarkAsDirtyPhys(page* pg);
OBOS_EXPORT void Mm_MarkAsStandbyPhys(page* pg);
void Mm_InitializePageWriter();
// Wakes up the page writer to free up memory
// Set 'wait' to true to wait for the page writer to release
void Mm_WakePageWriter(bool wait);
irql Mm_TakeSwapLock();
void Mm_ReleaseSwapLock(irql oldIrql);

obos_status Mm_ChangeSwapProvider(swap_dev* to);
