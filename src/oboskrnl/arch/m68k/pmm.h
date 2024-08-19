/*
	oboskrnl/arch/x86_64/pmm.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <arch/m68k/loader/Limine.h>

typedef struct limine_memmap_entry obos_pmem_map_entry;
#define pmem_map_base base
#define pmem_map_size length
#define pmem_map_type type
#define PHYSICAL_MEMORY_TYPE_USABLE LIMINE_MEMMAP_USABLE
#define PHYSICAL_MEMORY_TYPE_RECLAIMABLE LIMINE_MEMMAP_ACPI_RECLAIMABLE
#define PHYSICAL_MEMORY_TYPE_LOADER_RECLAIMABLE LIMINE_MEMMAP_LOADER_RECLAIMABLE

void* Arch_MapToHHDM(uintptr_t phys);
uintptr_t Arch_UnmapFromHHDM(void* virt);