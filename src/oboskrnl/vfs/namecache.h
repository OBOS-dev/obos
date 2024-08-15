/*
 * oboskrnl/vfs/namecache.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/tree.h>
#include <utils/list.h>

#include <uacpi_libc.h>

typedef RB_HEAD(namecache, namecache_ent) namecache;
typedef LIST_HEAD(namecache_list, struct namecache_ent) namecache_list;

typedef struct namecache_ent
{
    RB_ENTRY(namecache_ent) rb_cache;
    LIST_NODE(namecache_list, struct namecache_ent) list_node;
    struct vnode* ref;
    char *path;
} namecache_ent;

inline static int cmp_namecache_ent(const struct namecache_ent* a, const struct namecache_ent* b)
{
    return uacpi_strcmp(a->path, b->path);
}
RB_PROTOTYPE(namecache, namecache_ent, rb_cache, cmp_namecache_ent);
LIST_PROTOTYPE(namecache_list, struct namecache_ent, list_node);