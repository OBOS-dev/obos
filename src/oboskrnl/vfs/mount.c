/*
 * oboskrnl/vfs/mount.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <utils/list.h>

#include <vfs/namecache.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/mount.h>
#include <vfs/alloc.h>

#include <driver_interface/header.h>

#include <locks/mutex.h>

#include <utils/tree.h>
#include <utils/list.h>
#include <utils/string.h>

struct dirent* Vfs_Root;
mount_list Vfs_Mounted;

typedef LIST_HEAD(symbolic_link_list, struct symbolic_link) symbolic_link_list;
typedef struct symbolic_link
{
    dirent* ent;
    dev_desc desc;
    LIST_NODE(symbolic_link_list, struct symbolic_link) node;
} symbolic_link;
LIST_GENERATE_STATIC(symbolic_link_list, struct symbolic_link, node);

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
OBOS_STATIC_ASSERT(sizeof(driver_file_perm) == sizeof(file_perm), "Invalid sizes!");
static vnode* create_vnode(mount* mountpoint, dev_desc desc, file_type* t)
{
    file_type type = 0;
    driver_file_perm perm = {};
    mountpoint->fs_driver->driver->header.ftable.get_file_perms(desc, &perm);
    mountpoint->fs_driver->driver->header.ftable.get_file_type(desc, &type);
    vnode* vn = Vfs_Calloc(1, sizeof(vnode));
    switch (type)
    {
        case FILE_TYPE_REGULAR_FILE:
            vn->vtype = VNODE_TYPE_REG;
            mountpoint->fs_driver->driver->header.ftable.get_max_blk_count(desc, &vn->filesize);
            break;
        case FILE_TYPE_DIRECTORY:
            vn->vtype = VNODE_TYPE_DIR;
            break;
        case FILE_TYPE_SYMBOLIC_LINK:
        {
            // Defer the initialization of the vnode.
            Vfs_Free(vn);
            vn = nullptr;
            break;
        }
        default:
            OBOS_ASSERT(type);
    }
    vn->mount_point = mountpoint;
    vn->desc = desc;
    memcpy(&vn->perm, &perm, sizeof(file_perm));
    if (t)
        *t = type;
    return vn;
}
static iterate_decision callback(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata)
{
    uintptr_t *udata = (uintptr_t*)userdata;
    mount* mountpoint = (mount*)udata[0];
    vdev* fs_driver = (vdev*)udata[1];
    vdev* device = (vdev*)udata[2];
    symbolic_link_list* symlinks = (symbolic_link_list*)udata[3];
    OBOS_UNUSED(blkSize);
    OBOS_UNUSED(device);
    file_type type = 0;
    const char* path = nullptr;
    OBOS_UNUSED(mountpoint);
    OBOS_UNUSED(desc);
    OBOS_UNUSED(blkCount);
    OBOS_UNUSED(device);
    fs_driver->driver->header.ftable.query_path(desc, &path);
    fs_driver->driver->header.ftable.get_file_type(desc, &type);
    size_t pathlen = strlen(path);
    const char* tok = path;
    bool is_base = false;
    size_t tok_len = 0;
    {
        size_t currentPathLen = strlen(tok)-1;
        if (tok[currentPathLen] != '/')
            currentPathLen++;
        while (tok[currentPathLen] == '/')
            currentPathLen--;
        tok_len = strchr(tok, '/');
        if (tok_len != currentPathLen)
            tok_len--;
        else
            is_base = true;
    }
    dirent* last = mountpoint->root;
    char* currentPath = Vfs_Calloc(pathlen + 1, sizeof(char));
    size_t currentPathLen = 0;
    OBOS_Debug("%s\n", path);
    while (tok < (path+pathlen))
    {
        char* token = Vfs_Calloc(tok_len + 1, sizeof(char));
        memcpy(token, tok, tok_len);
        token[tok_len] = 0;
        memcpy(currentPath + currentPathLen, token, tok_len);
        currentPathLen += tok_len;
        if (!is_base)
            currentPath[currentPathLen++] = '/';
        dirent* new = VfsH_DirentLookupFrom(token, last ? last : mountpoint->root);
        if (!new)
        {
            // Allocate a new dirent.
            new = Vfs_Calloc(1, sizeof(dirent));
            OBOS_InitStringLen(&new->name, token, tok_len);
            dev_desc curdesc = 0;
            file_type curtype = 0;
            if (is_base)
                curdesc = desc;
            else
                fs_driver->driver->header.ftable.path_search(&curdesc, currentPath);
            mountpoint->fs_driver->driver->header.ftable.get_file_type(desc, &type);
            if (curtype == FILE_TYPE_SYMBOLIC_LINK)
            {
                symbolic_link* lnk = Vfs_Calloc(1, sizeof(symbolic_link));
                lnk->ent = new;
                lnk->desc = desc;
                LIST_APPEND(symbolic_link_list, symlinks, lnk);   
            }
            else 
            {
                vnode* new_vn = create_vnode(mountpoint, curdesc, &curtype);
                new->vnode = new_vn;
                new->vnode->refs++;
            }
        }
        if (!new->d_prev_child && !new->d_next_child && last->d_children.head != new)
            VfsH_DirentAppendChild(last ? last : mountpoint->root, new);
        last = new;
        Vfs_Free(token);

        tok += str_search(tok, '/');
        size_t currentPathLen = strlen(tok)-1;
        if (tok[currentPathLen] != '/')
            currentPathLen++;
        while (tok[currentPathLen] == '/')
            currentPathLen--;
        tok_len = strchr(tok, '/');
        if (tok_len != currentPathLen)
            tok_len--;
        else
            is_base = true;
    }
    if (type == FILE_TYPE_DIRECTORY)
        fs_driver->driver->header.ftable.list_dir(desc, callback, udata);
    return ITERATE_DECISION_CONTINUE;
}
obos_status Vfs_Mount(const char* at_, vdev* device, vdev* fs_driver, mount** pMountpoint)
{
    OBOS_UNUSED(device);
    if (!Vfs_Root)
        return OBOS_STATUS_INVALID_INIT_PHASE;
    if (!at_ || !fs_driver)
        return OBOS_STATUS_INVALID_ARGUMENT;
    dirent* at = VfsH_DirentLookup(at_);
    if (!at)
        return OBOS_STATUS_NOT_FOUND;
    if (at->vnode->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_OPERATION;
    if (at->vnode->flags & VFLAGS_MOUNTPOINT)
        return OBOS_STATUS_ALREADY_MOUNTED;
    mount* mountpoint = Vfs_Calloc(1, sizeof(mount));
    if (pMountpoint)
        *pMountpoint = mountpoint;
    mountpoint->mounted_on = at->vnode;
    at->vnode->un.mounted = mountpoint;
    at->vnode->flags |= VFLAGS_MOUNTPOINT;
    mountpoint->root = at;
    symbolic_link_list symlinks = {};
    uintptr_t udata[4] = {
        (uintptr_t)mountpoint,
        (uintptr_t)fs_driver,
        (uintptr_t)device,
        (uintptr_t)&symlinks,
    };
    mountpoint->fs_driver = memcpy(Vfs_Calloc(1, sizeof(vdev)), fs_driver, sizeof(*fs_driver));
    if (device)
        mountpoint->device = memcpy(Vfs_Calloc(1, sizeof(vdev)), device, sizeof(*device));
    fs_driver->driver->header.ftable.list_dir(UINTPTR_MAX, callback, udata);
    for (symbolic_link* lnk = LIST_GET_HEAD(symbolic_link_list, &symlinks); lnk; )
    {
        symbolic_link* next = LIST_GET_NEXT(symbolic_link_list, &symlinks, lnk);
        if (lnk->ent->vnode)
        {
            // We have already been resolved.
            lnk = next;
            continue;
        }
        const char *points_at = nullptr;
        dev_desc desc_points_at = 0;
        fs_driver->driver->header.ftable.get_linked_desc(lnk->desc, &desc_points_at);
        fs_driver->driver->header.ftable.query_path(desc_points_at, &points_at);
        dirent* resolved = VfsH_DirentLookupFrom(points_at, mountpoint->root);
        OBOS_ASSERT(resolved); // TODO: Proper error handling.
        dev_desc desc = 0;
        while (!resolved->vnode)
        {
            desc = desc_points_at;
            // *resolved is probably also an unresolved symlink.
            // resolve it.
            points_at = nullptr;
            desc_points_at = 0;
            fs_driver->driver->header.ftable.get_linked_desc(desc, &desc_points_at);
            fs_driver->driver->header.ftable.query_path(desc_points_at, &points_at);
            resolved = VfsH_DirentLookupFrom(points_at, mountpoint->root);
            OBOS_ASSERT(resolved); // TODO: Proper error handling.
        }
        lnk->ent->vnode = resolved->vnode;
        lnk->ent->vnode->refs++;
        Vfs_Free(lnk); // free the temporary structure.
        lnk = next;
    }
    LIST_APPEND(mount_list, &Vfs_Mounted, mountpoint);
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_Unmount(mount* what)
{
    OBOS_UNUSED(what);
    return OBOS_STATUS_UNIMPLEMENTED;
}
obos_status Vfs_UnmountP(const char* at)
{
    dirent* resolved = VfsH_DirentLookup(at);
    if (!resolved)
        return OBOS_STATUS_NOT_FOUND;
    if (resolved->vnode->flags & VFLAGS_MOUNTPOINT)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return Vfs_Unmount(resolved->vnode->un.mounted);
}

RB_GENERATE(namecache, namecache_ent, rb_cache, cmp_namecache_ent);
LIST_GENERATE(mount_list, mount, node);
