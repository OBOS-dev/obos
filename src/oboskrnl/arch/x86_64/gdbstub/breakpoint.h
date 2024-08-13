/*
 * oboskrnl/arch/x86_64/gdbstub/breakpoint.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <utils/list.h>

typedef LIST_HEAD(sw_breakpoint_list, struct sw_breakpoint) sw_breakpoint_list;
LIST_PROTOTYPE(sw_breakpoint_list, struct sw_breakpoint, node);

typedef struct sw_breakpoint
{
    uintptr_t addr;
    uint8_t at; // the byte at addr before setting it to a breakpoint instruction
    LIST_NODE(sw_breakpoint_list, struct sw_breakpoint) node;
} sw_breakpoint;