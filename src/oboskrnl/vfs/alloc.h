/*
 * oboskrnl/vfs/alloc.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <allocators/base.h>

extern allocator_info* Vfs_Allocator;

OBOS_EXPORT void  Vfs_Free(void* what);
#ifdef __clang__
OBOS_EXPORT void* Vfs_Malloc(size_t cnt);
OBOS_EXPORT void* Vfs_Calloc(size_t nObjs, size_t szObj);
#else
OBOS_EXPORT __attribute__((malloc, malloc(Vfs_Free, 1), returns_nonnull)) void* Vfs_Malloc(size_t cnt);
OBOS_EXPORT __attribute__((malloc, malloc(Vfs_Free, 1), returns_nonnull)) void* Vfs_Calloc(size_t nObjs, size_t szObj);
#endif
OBOS_EXPORT void* Vfs_Realloc(void* what, size_t cnt);