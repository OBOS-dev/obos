/*
 * oboskrnl/mm/sglist.h
 *
 * Copyright (c) 2026 Omar Berrow
*/

#pragma once

#include <int.h>

#include <mm/context.h>

struct physical_region
{
	uintptr_t phys;
	size_t sz;	
};

OBOS_EXPORT obos_status DrvH_ScatterGather(context* ctx, void* base, size_t size, struct physical_region** regions, size_t* nRegions, size_t maxRegionCount, bool rw);
OBOS_EXPORT obos_status DrvH_FreeScatterGatherList(context* ctx, void* base, size_t size, struct physical_region* regions, size_t nRegions);