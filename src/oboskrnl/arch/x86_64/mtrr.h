/*
 * oboskrnl/arch/x86_64/mtrr.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

// Only to be called once on the BSP
void Arch_SaveMTRRs();
// Can be called on any CPU any amount of times.
void Arch_RestoreMTRRs();
