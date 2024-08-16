/*
 * oboskrnl/vfs/pagecache.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/list.h>

typedef LIST_HEAD(pagecache, struct pagecache_ent) pagecache;
LIST_PROTOTYPE(pagecache, struct pagecache_ent, node);

typedef struct pagecache_ent
{
    char* data;
    size_t sz;
    size_t fileoff;
    bool dirty;
    LIST_NODE(pagecache, struct pagecache_ent) node;
} pagecache_ent;