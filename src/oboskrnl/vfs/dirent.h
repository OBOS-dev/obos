/*
 * oboskrnl/vfs/dirent.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/limits.h>

#include <utils/string.h>
#include <utils/list.h>

typedef struct dirent
{
    struct
    {
        struct dirent* parent;
        struct
        {
            struct dirent* head;
            struct dirent* tail;
            size_t nChildren;
        } children;
        struct dirent* next_child;
        struct dirent* prev_child;
    } tree_info;
    struct vnode* vnode;
    string name;
} dirent;
#define d_children tree_info.children
#define d_next_child tree_info.next_child
#define d_prev_child tree_info.prev_child
#define d_parent tree_info.parent
void VfsH_DirentAppendChild(dirent* parent, dirent* child);
void VfsH_DirentRemoveChild(dirent* parent, dirent* what);
// path shouldn't have unneeded slashes, this way, there is a higher chance of a name cache hit, thus speeding up
// the lookup
dirent* VfsH_DirentLookup(const char* path);
dirent* VfsH_DirentLookupFrom(const char* path, dirent* root);

OBOS_EXPORT dirent* Drv_RegisterVNode(struct vnode* vn, const char* const dev_name);