/*
 * oboskrnl/net/lo.h
 *
 * Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/vnode.h>

void Net_InitializeLoopbackDevice();

extern vnode* Net_LoopbackDevice;