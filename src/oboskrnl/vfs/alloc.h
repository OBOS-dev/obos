/*
 * oboskrnl/vfs/alloc.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/base.h>

OBOS_EXPORT extern allocator_info* Vfs_Allocator;

void* Vfs_Malloc(size_t cnt);
void* Vfs_Calloc(size_t nObjs, size_t szObj);
void* Vfs_Realloc(void* what, size_t cnt);
void  Vfs_Free(void* what);