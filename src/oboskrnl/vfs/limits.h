/*
 * oboskrnl/vfs/limits.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
 *
 * Defines limits for the VFS.
*/

#pragma once

#include <int.h>

#define MAX_FILENAME_LEN 256UL /* In bytes */

typedef  intptr_t off_t;
typedef uintptr_t uoff_t;
#define OFF_T_MIN INT64_MIN
#define OFF_T_MAX INT64_MAX
#define UOFF_T_MIN UINT64_MIN
#define UOFF_T_MAX UINT64_MAX
typedef enum whence_t
{
    SEEK_SET,
    SEEK_CUR,
    SEEK_END,
} whence_t;
