/*
 * oboskrnl/init_proc.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <execve.h>

void OBOS_LoadInit();

OBOS_NORETURN OBOS_WEAK void OBOSS_HandOffToInit(struct exec_aux_values* info);
