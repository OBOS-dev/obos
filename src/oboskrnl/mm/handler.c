/*
 * oboskrnl/mm/handler.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/handler.h>
#include <mm/context.h>
#include <mm/page.h>

obos_status Mm_HandlePageFault(context* ctx, uintptr_t addr, uint32_t ec)
{
    page* page = ;
}
obos_status Mm_RunPRE(context* ctx)
{

}