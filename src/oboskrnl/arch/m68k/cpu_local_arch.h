/*
 * oboskrnl/arch/m68k/cpu_local_arch.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

// deferred irq
typedef struct m68k_dirq
{
    struct m68k_dirq *next, *prev;
    size_t nDefers;
    // Is called after the defer happens
    void(*on_defer_callback)(void* udata); // used by the PIC code.
    void* udata;
    uint8_t irql;
} m68k_dirq;
typedef struct m68k_dirq_list
{
    m68k_dirq *head, *tail;
    size_t nNodes;
} m68k_dirq_list;
typedef struct cpu_local_arch
{
    m68k_dirq irqs[256];
    m68k_dirq_list deferred;
} cpu_local_arch;