/*
* oboskrnl/net/tables.h
*
* Copyright (c) 2025 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <driver_interface/header.h>

#include <vfs/vnode.h>

#include <scheduler/thread.h>

#include <net/eth.h>

typedef struct net_tables {
    thread* dispatch_thread;
    bool kill_dispatch;
    mac_address mac;
    dev_desc desc;
} net_tables;

obos_status Net_Initialize(vnode* nic);