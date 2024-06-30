/*
 * oboskrnl/mm/init.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdint.h>

#include <mm/prot.h>

/// <summary>
/// Initializes the VMM.
/// </summary>
void Mm_Initialize();
/// <summary>
/// Initializes the VMM allocator (non-paged pool allocator).
/// </summary>
void Mm_InitializeAllocator();
/// <summary>
/// Returns whether the VMM is initialized or not.
/// </summary>
/// <returns>Whether the VMM is initialized or not.</returns>
bool Mm_Initialized();