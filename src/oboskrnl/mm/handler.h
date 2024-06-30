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
/// Ages pages in a context, and puts pages into the working-set.
/// </summary>
/// <param name="ctx">A pointer to the context. Cannot be nullptr.</param>
/// <returns>The status of the function.</returns>
obos_status Mm_AgePagesInContext(context* ctx);
/// <summary>
/// The page fault callback.
/// </summary>
/// <param name="ctx">A pointer to the context. Cannot be nullptr.</param>
/// <param name="ec">The page fault's error code.</param>
/// <param name="addr">The virtual address that faulted.</param>
/// <returns>OBOS_STATUS_SUCCESS if the page fault was handled, OBOS_STATUS_UNHANDLED if the page fault went unhandled, otherwise an error.</returns>
obos_status Mm_OnPageFault(context* ctx, uint32_t ec, uintptr_t addr);

/// <summary>
/// <para>Locks the page fault handler.</para>
/// </summary>
/// <param name="lock">Whether it should be locked or unlocked.</param>
void MmS_LockPageFaultHandler(bool lock);