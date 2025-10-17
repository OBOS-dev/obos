/*
 * oboskrnl/vfs/mouse.h
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Use multi-line comments here only, this file might get copied to obos' mlibc sysdep.
*/

#pragma once

#include <int.h>

typedef struct mouse_packet {
    int x,y,z;
    bool rb : 1;
    bool lb : 1;
    bool mb : 1;
    bool b4 : 1;
    bool b5 : 1;
} mouse_packet;