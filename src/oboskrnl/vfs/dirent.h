/*
 * oboskrnl/vfs/dirent.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/limits.h>

#include <utils/string.h>

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
dirent* VfsH_DirentLookup(const char* path);