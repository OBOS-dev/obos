/*
 * oboskrnl/arch/x86_64/irq_vector.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>

#include <irq/irql.h>

typedef uint8_t irq_vector_id;
#define OBOS_IRQ_VECTOR_ID_MAX (224 /* 256 vectors - the 32 reserved vectors */)
#define OBOS_IRQ_VECTOR_ID_TO_IRQL(x) ((irql)(((x)>>4)+2))
#define OBOS_IRQL_TO_IRQ_VECTOR_ID(x) ((irq_vector_id)(((x)<<4)-0x20))
#define OBOS_IRQ_VECTOR_ID_COUNT_PER_IRQL (16)