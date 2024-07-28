/*
 * oboskrnl/mm/handler.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>

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

/// <summary>
/// The page fault callback. Can be only be called when the memory-manager is initialized.
/// </summary>
/// <param name="ctx">The context where the fault happened.</param>
/// <param name="addr">The virtual address that faulted.</param>
/// <param name="ec">The page fault's error code.</param>
/// <returns>OBOS_STATUS_SUCCESS if the page fault was handled, OBOS_STATUS_UNHANDLED if the page fault went unhandled, otherwise an error.</returns>
obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec);
/// <summary>
/// Runs the page replacement algorithm on pages in a context.<para/>
/// This essentially chooses pages from within the context, and puts them within the working-set of the context.
/// </summary>
/// <param name="ctx">A pointer to the context. Cannot be nullptr.</param>
/// <returns>The status of the function.</returns>
obos_status Mm_RunPRA(context* ctx);