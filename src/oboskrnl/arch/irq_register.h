#pragma once

#if defined(__x86_64__) || defined(_WIN64)
#include <arch/x86_64/irq/idt.h>
#endif

#ifndef OBOS_MAX_INTERRUPT_VECTORS
#error OBOS_MAX_INTERRUPT_VECTORS is not defined by the architecture
#endif
#ifndef OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL
#error OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL is not defined by the architecture
#endif
#ifndef OBOS_IRQL_TO_VECTOR
#include <todo.h>
COMPILE_MESSAGE("OBOS_IRQL_TO_VECTOR is not defined. Defining automatically...\n");
#define OBOS_IRQL_TO_VECTOR(irql) ((irql) >= 2 ? ((irql) * OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL) : 0)
#endif
static_assert(OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL < OBOS_MAX_INTERRUPT_VECTORS, "");
static_assert((OBOS_MAX_INTERRUPT_VECTORS / OBOS_MAX_INTERRUPT_VECTORS_PER_IRQL) != 0, "");