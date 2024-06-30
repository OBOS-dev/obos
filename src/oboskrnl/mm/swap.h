/*
 * oboskrnl/mm/swap.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/pg_node.h>
#include <stdint.h>

typedef struct swap_dev
{
    /// <summary>
    /// Puts a page into swap.
    /// </summary>
    /// <param name="node">The page to put into swap. The page mustn't be mapped.</param>
    /// <returns>The status of the function.</returns>
    obos_status(*put_page)(struct swap_dev* dev, const page_node* node, uintptr_t phys);
    /// <summary>
    /// Puts a page from swap back into physical memory.
    /// </summary>
    /// <param name="node">The page to retrieve from swap. The page mustn't be mapped.</param>
    /// <returns>The status of the function.</returns>
    obos_status(*get_page)(struct swap_dev* dev, const page_node* node, uintptr_t phys);
    // Metadata for the swap device.
    void* data;
} swap_dev;

// The page should be locked before calling these functions.
// TODO: Document the functions.

obos_status Mm_PageOutPage(swap_dev* swap, page_node* page);
obos_status Mm_PageInPage(swap_dev* swap, page_node* page);

extern swap_dev* Mm_SwapProvider;
// Must be initialized by the architecture before initialization of the VMM.
extern void* MmS_InitialSwapBuffer;
extern size_t MmS_InitialSwapBufferSize;