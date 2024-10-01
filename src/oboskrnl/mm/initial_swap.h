/*
 * oboskrnl/mm/initial_swap.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/swap.h>

obos_status Mm_InitializeInitialSwapDevice(swap_dev* dev, size_t size);