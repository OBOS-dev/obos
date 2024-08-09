/*
 * oboskrnl/arch/x86_64/gdbstub/alloc.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

// All memory for the gdb stub is allocated using the non-paged pool allocator

void* Kdbg_Malloc(size_t sz);
void* Kdbg_Calloc(size_t nObjs, size_t szObj);
void* Kdbg_Realloc(void* ptr, size_t newSz);
void  Kdbg_Free(void* ptr);