/*
 * oboskrnl/mm/handler.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>
#include <mm/page.h>

enum
{
    /// <summary>
    /// If set, the page fault happened on a present page.
    /// </summary>
    PF_EC_PRESENT = BIT(0),
    /// <summary>
    /// If set, the page fault happened on a write access, otherwise, it's rather a read access, or an instruction fetch.
    /// </summary>
    PF_EC_RW = BIT(1),
    /// <summary>
    /// If set, the page fault happened in user space.
    /// </summary>
    PF_EC_UM = BIT(2),
    /// <summary>
    /// If set, the page fault happened on an instruction fetch.
    /// </summary>
    PF_EC_EXEC = BIT(4),
    /// <summary>
    /// There was an invalid page table entry during translation.
    /// </summary>
    PF_EC_INV_PTE = BIT(5),
};
typedef enum {
    INVALID_FAULT,
    // A soft fault is when a swap in could use the dirty or standby lists to do a swap in, the part of the page cache at the file offset
    // of the address was already filled in, or the fault was a CoW fault.
    SOFT_FAULT,
    // A hard fault is when a swap in needed to read from the swap_dev, or a part of the page cache needed to be read from disk
    // to satisfy the fault.
    HARD_FAULT,
    // The page fault was caused by an access violation.
    ACCESS_FAULT,
} fault_type;

/// <summary>
/// The page fault callback. Can be only be called when the memory-manager is initialized.
/// </summary>
/// <param name="ctx">The context where the fault happened.</param>
/// <param name="addr">The virtual address that faulted.</param>
/// <param name="ec">The page fault's error code.</param>
/// <returns>OBOS_STATUS_SUCCESS if the page fault was handled, OBOS_STATUS_UNHANDLED if the page fault went unhandled, otherwise an error.</returns>
OBOS_EXPORT obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec);
/// <summary>
/// Runs the page replacement algorithm on pages in a context.<para/>
/// This essentially chooses pages from within the context, and puts them within the working-set of the context.
/// </summary>
/// <param name="ctx">A pointer to the context. Cannot be nullptr.</param>
/// <returns>The status of the function.</returns>
obos_status Mm_RunPRA(context* ctx);

void MmH_RemovePageFromWorkingset(context* ctx, working_set_node* node);