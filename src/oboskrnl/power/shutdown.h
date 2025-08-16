/*
 * oboskrnl/power/shutdown.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

OBOS_NORETURN OBOS_EXPORT void OBOS_Shutdown();
OBOS_NORETURN OBOS_EXPORT void OBOS_Reboot();
