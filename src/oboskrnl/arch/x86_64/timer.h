/*
 * oboskrnl/arch/x86_64/timer.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

// NOTE: To be called at > IRQL_PASSIVE.
void Arch_InitializeSchedulerTimer();
