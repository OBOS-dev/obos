/*
 * oboskrnl/mm/fork.h
 *
 * Copyright (c) 2024 Omar Berrow
 */

#pragma once

#include <int.h>
#include <error.h>

#include <mm/context.h>

obos_status Mm_ForkContext(context* into, context* toFork);
