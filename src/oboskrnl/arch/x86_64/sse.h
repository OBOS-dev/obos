/*
 * oboskrnl/arch/x86_64/sse.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

void* Arch_AllocateXSAVERegion();
void  Arch_FreeXSAVERegion(void* reg);
// Enables stuff such as XSAVE, SSE(2), AVX, AVX512, etc.
void Arch_EnableSIMDFeatures();
size_t Arch_GetXSaveRegionSize();

// Set to false if the thread context code should fallback to fxsave/fxrstor,
// otherwise, the thread context code can and should use xsave/xrstor
extern bool Arch_HasXSAVE;
