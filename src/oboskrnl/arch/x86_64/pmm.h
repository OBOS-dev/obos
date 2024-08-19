/*
	oboskrnl/arch/x86_64/pmm.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <UltraProtocol/ultra_protocol.h>

typedef struct ultra_memory_map_entry obos_pmem_map_entry;
#define pmem_map_base physical_address
#define pmem_map_size size
#define pmem_map_type type
#define PHYSICAL_MEMORY_TYPE_USABLE ULTRA_MEMORY_TYPE_FREE
#define PHYSICAL_MEMORY_TYPE_RECLAIMABLE ULTRA_MEMORY_TYPE_RECLAIMABLE
#define PHYSICAL_MEMORY_TYPE_LOADER_RECLAIMABLE ULTRA_MEMORY_TYPE_LOADER_RECLAIMABLE

void* Arch_MapToHHDM(uintptr_t phys);
uintptr_t Arch_UnmapFromHHDM(void* virt);