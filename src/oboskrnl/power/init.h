/*
 * oboskrnl/power/init.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

void OBOS_SetupEarlyTableAccess();
void OBOS_InitializeUACPI();

void OBOS_InitializeECFromECDT();
void OBOS_InitializeECFromNamespace();
