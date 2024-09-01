/*
 * oboskrnl/mm/swap.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/page.h>

typedef struct swap_device
{
    obos_status(* swap_resv)(struct swap_device* dev, uintptr_t *id, size_t nPages);
    obos_status(* swap_free)(struct swap_device* dev, uintptr_t  id, size_t nPages);
    obos_status(*swap_write)(struct swap_device* dev, uintptr_t  id, uintptr_t phys, size_t nPages, size_t offsetBytes);
    obos_status(* swap_read)(struct swap_device* dev, uintptr_t  id, uintptr_t phys, size_t nPages, size_t offsetBytes);
    obos_status(*deinit_dev)(struct swap_device* dev);
    void* metadata;
} swap_dev;
extern swap_dev* Mm_SwapProvider;

obos_status Mm_SwapOut(page* page);
obos_status Mm_SwapIn(page* page);

obos_status Mm_ChangeSwapProvider(swap_dev* to);