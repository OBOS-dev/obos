/*
 * drivers/generic/fat/lookup.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>

#include "structs.h"

// Adapated from vfs/dirent.c

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
static fat_dirent_cache* on_match(fat_dirent_cache** const curr_, fat_dirent_cache** const root, const char** const tok, size_t* const tok_len, const char** const path, 
                        size_t* const path_len)
{
    fat_dirent_cache *curr = *curr_;
    *root = curr;
    const char *newtok = (*tok) + str_search(*tok, '/');
    if (newtok >= (*path + *path_len))
        return curr;
    if (!curr->fdc_children.nChildren)
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
    while ((*tok)[(*tok_len) - 1] == '/')
        (*tok_len)--;
    return nullptr;
}
fat_dirent_cache* DirentLookupFrom(const char* path, fat_dirent_cache* root)
{
    if (!path)
        return nullptr;
    size_t path_len = strlen(path);
    if (!path_len)
        return nullptr;
    for (; *path == '/'; path++, path_len--)
        ;
    const char* tok = path;
    size_t tok_len = strchr(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    while (tok[tok_len - 1] == '/')
        tok_len--;
    if (!tok_len)
        return nullptr;
    while(root)
    {
        fat_dirent_cache* curr = root;
        if (OBOS_CompareStringNC(&root->name, tok, tok_len))
        {
            // Match!
            root = curr->fdc_children.head ? curr->fdc_children.head : root;
            fat_dirent_cache* what = 
                on_match(&curr, &root, &tok, &tok_len, &path, &path_len);
            if (what)
                return what;
            continue;
        }
        for (curr = root->fdc_children.head; curr;)
        {
            if (OBOS_CompareStringNC(&curr->name, tok, tok_len))
            {
                // Match!
                fat_dirent_cache* what = 
                    on_match(&curr, &root, &tok, &tok_len, &path, &path_len);
                if (what)
                    return what;
                // else
                //     return nullptr;
                curr = curr->fdc_children.head ? curr->fdc_children.head : curr;
                break;
            }

            // root = curr->fdc_children.head ? curr->fdc_children.head : root;
            curr = curr->fdc_next_child;
        }
        if (!curr)
            root = root->fdc_parent;
    }
    return nullptr;
}