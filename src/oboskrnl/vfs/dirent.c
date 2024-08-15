/*
 * oboskrnl/vfs/dirent.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>

#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>
#include <vfs/namecache.h>
#include <vfs/limits.h>

#include <utils/string.h>
#include <utils/list.h>
#include <utils/tree.h>

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
static namecache_ent* namecache_lookup_internal(namecache* nc, const char* path)
{
    namecache_ent what = { };
    OBOS_InitString(&what.path, path);
    namecache_ent* hit = RB_FIND(namecache, nc, &what);
    OBOS_FreeString(&what.path);
    return hit;
}
static dirent* namecache_lookup(namecache* nc, const char* path)
{
    namecache_ent* nc_ent = namecache_lookup_internal(nc, path);
    if (!nc_ent)
        return nullptr;
    return nc_ent->ent;
}
static void namecache_insert(namecache* nc, dirent* what, const char* path, size_t pathlen)
{
    namecache_ent* ent = Vfs_Calloc(1, sizeof(namecache_ent));
    ent->ent = what;
    ent->ref = what->vnode;
    OBOS_InitStringLen(&ent->path, path, pathlen);
    RB_INSERT(namecache, nc, ent);
}
static dirent* on_match(dirent** const curr_, dirent** const root, const char** const tok, size_t* const tok_len, const char** const path, 
                        size_t* const path_len, size_t* const lastMountPoint, mount** const lastMount)
{
    dirent *curr = *curr_;
    *root = curr;
    const char *newtok = (*tok) + str_search(*tok, '/');
    if (newtok >= (*path + *path_len))
    {
        if (*lastMount)
            namecache_insert(&(*lastMount)->nc, curr, *path+(*lastMountPoint), strchr((*path)+(*lastMountPoint), '/')-1);
        return curr;
    }
    if (!curr->d_children.nChildren)
        return nullptr; // could not find node.
    *tok = newtok;
    size_t currentPathLen = strlen(*tok)-1;
    if ((*tok)[currentPathLen] != '/')
        currentPathLen++;
    while ((*tok)[currentPathLen] == '/')
        currentPathLen--;
    *tok_len = strchr(*tok, '/');
    if (*tok_len != currentPathLen)
        (*tok_len)--;
    if (curr->vnode && curr->vnode->flags & VFLAGS_MOUNTPOINT)
    {
        (*lastMountPoint) = ((*tok)-(*path));
        (*lastMount) = curr->vnode->un.mounted;
        char* nc_path = Vfs_Calloc(*tok_len + 1, sizeof(char));
        memcpy(nc_path, tok, *tok_len);
        dirent* hit = namecache_lookup(&curr->vnode->un.mounted->nc, newtok);
        Vfs_Free(nc_path);
        return hit;
    }
    return nullptr;
}
dirent* VfsH_DirentLookup(const char* path)
{
    if (!path)
        return nullptr;
    if (strcmp(path, "/"))
        return Vfs_Root;
    dirent* root = Vfs_Root;
    size_t path_len = strlen(path);
    if (!path_len)
        return nullptr;
    for (; *path == '/'; path++, path_len--)
        ;
    const char* tok = path;
    size_t tok_len = strchr(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    if (!tok_len)
        return nullptr;
    // Offset of the last mount point in the path.
    size_t lastMountPoint = 0; 
    mount* lastMount = Vfs_Root->vnode ? Vfs_Root->vnode->un.mounted : nullptr;
    while(root)
    {
        dirent* curr = root;
        if (OBOS_CompareStringNC(&root->name, tok, tok_len))
        {
            // Match!
            // root = curr;
            root = curr->d_children.head ? curr->d_children.head : root;
            dirent* what = 
                on_match(&curr, &root, &tok, &tok_len, &path, &path_len, &lastMountPoint, &lastMount);
            if (what)
                return what;
            continue;
        }
        for (curr = root->d_children.head; curr;)
        {
            if (OBOS_CompareStringNC(&curr->name, tok, tok_len))
            {
                // Match!
                dirent* what = 
                    on_match(&curr, &root, &tok, &tok_len, &path, &path_len, &lastMountPoint, &lastMount);
                if (what)
                    return what;
                curr = curr->d_children.head ? curr->d_children.head : curr;
                break;
            }

            // root = curr->d_children.head ? curr->d_children.head : root;
            curr = curr->d_next_child;
        }
        if (!curr)
            root = root->d_parent;
    }
    return nullptr;
}
void VfsH_DirentAppendChild(dirent* parent, dirent* child)
{
    if(!parent->d_children.head)
        parent->d_children.head = child;
    if (parent->d_children.tail)
        parent->d_children.tail->d_next_child = child;
    child->d_prev_child = parent->d_children.tail;
    parent->d_children.tail = child;
    parent->d_children.nChildren++;
}
void VfsH_DirentRemoveChild(dirent* parent, dirent* what)
{
    if (what->d_prev_child)
        what->d_prev_child->d_next_child = what->d_next_child;
    if (what->d_next_child)
        what->d_next_child->d_prev_child = what->d_prev_child;
    if (parent->d_children.head == what)
        parent->d_children.head = what->d_next_child;
    if (parent->d_children.tail == what)
        parent->d_children.tail = what->d_prev_child;
    parent->d_children.nChildren--;
    what->d_parent = nullptr; // we're now an orphan ):
}