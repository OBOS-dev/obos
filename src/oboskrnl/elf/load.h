/*
 * oboskrnl/elf/load.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>

obos_status OBOS_LoadELF(context* ctx, const void* file, size_t szFile);
