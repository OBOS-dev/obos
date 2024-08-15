/*
 * oboskrnl/vfs/dirent.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>
#include <vfs/limits.h>

#include <utils/string.h>

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
dirent* VfsH_DirentLookup(const char* path)
{
    if (!path)
        return nullptr;
    dirent* root = Vfs_Root;
    size_t path_len = strlen(path);
    if (!path_len)
        return nullptr;
    const char* tok = path;
    size_t tok_len = str_search(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    if (!tok_len)
        return nullptr;
    while(root)
    {
        dirent* curr = root;
        if (OBOS_CompareStringNC(&root->name, tok, tok_len))
        {
            // Match!
            // Traverse one node down if there is more of the path left, otherwise return the current node.
            root = curr;
            const char *newtok = tok + tok_len + (tok_len != path_len ? 1 : 0);
            if (newtok >= (path + path_len))
                return root;
            if (!curr->d_children.nChildren)
                return nullptr; // could not find node.
            tok = newtok;
            tok_len = str_search(tok, '/');
            if (tok_len != path_len)
                tok_len--;
            break;
        }
        for (curr = root->d_children.head; curr;)
        {
            // Match!
            // Traverse one node down if there is more of the path left, otherwise return the current node.
            if (OBOS_CompareStringNC(&curr->name, tok, tok_len))
            {
                root = curr;
                const char *newtok = tok + tok_len + (tok_len != path_len ? 1 : 0);
                if (newtok >= (path + path_len))
                    return curr;
                if (!curr->d_children.nChildren)
                    return nullptr; // could not find node.
                tok = newtok;
                tok_len = str_search(tok, '/');
                if (tok_len != path_len)
                    tok_len--;
                break;
            }
            

            curr = curr->d_next_child;
        }
        if (!curr)
            root = root->d_parent;
    }
    return nullptr;
}
void VfsH_DirentAppendChild(dirent* parent, dirent* child)
{
    if (!parent->d_children.head)
        parent->d_children.head = child;
    if (!parent->d_children.tail)
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