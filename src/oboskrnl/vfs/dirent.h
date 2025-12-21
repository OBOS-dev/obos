/*
 * oboskrnl/vfs/dirent.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <vfs/limits.h>

#include <utils/string.h>
#include <utils/list.h>

enum {
    // This dirent hard links to the controlling tty of the process
    DIRENT_REFERS_CTTY = BIT(0),
};

typedef LIST_HEAD(dirent_list, struct dirent) dirent_list;
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
    int flags;
    LIST_NODE(dirent_list, struct dirent) node;
} dirent;
LIST_PROTOTYPE(dirent_list, dirent, node);
#define d_children tree_info.children
#define d_next_child tree_info.next_child
#define d_prev_child tree_info.prev_child
#define d_parent tree_info.parent
OBOS_EXPORT void VfsH_DirentAppendChild(dirent* parent, dirent* child);
OBOS_EXPORT void VfsH_DirentRemoveChild(dirent* parent, dirent* what);
// path shouldn't have unneeded slashes, this way, there is a higher chance of a name cache hit, thus speeding up
// the lookup
OBOS_EXPORT dirent* VfsH_DirentLookup(const char* path);
OBOS_EXPORT dirent* VfsH_DirentLookupFrom(const char* path, dirent* root);
OBOS_EXPORT dirent* VfsH_DirentLookupWD(const char* path, dirent* wd);
OBOS_EXPORT dirent* VfsH_DirentLookupFromCacheOnly(const char* path, dirent* root);
dirent* VfsH_FollowLink(dirent* ent);

char* VfsH_DirentPath(dirent* ent, dirent* relative_to);
char* VfsH_DirentPathKAlloc(dirent* ent, dirent* relative_to);

obos_status VfsH_Chdir(void* /* struct process */ target, const char *path);
obos_status VfsH_ChdirEnt(void* /* struct process */ target, dirent* ent);

OBOS_EXPORT dirent* Drv_RegisterVNode(struct vnode* vn, const char* const dev_name);

// solely for mlibc support
obos_status Vfs_ReadEntries(dirent* dent, void* buffer, size_t szBuf, dirent** last, size_t* nRead);
void Vfs_PopulateDirectory(dirent* dent);
