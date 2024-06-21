/*
 * oboskrnl/mm/init.h
 * 
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>
#include <stdint.h>

#include <mm/prot.h>

/// <summary>
/// Initializes the kernel context.
/// </summary>
void Mm_Initialize();