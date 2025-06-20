/*
 * oboskrnl/vfs/mount.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <memmanip.h>
#include <error.h>

#include <utils/list.h>

#include <vfs/pagecache.h>
#include <vfs/fd.h>
#include <vfs/namecache.h>
#include <vfs/vnode.h>
#include <vfs/dirent.h>
#include <vfs/mount.h>
#include <vfs/alloc.h>

#include <driver_interface/header.h>

#include <locks/mutex.h>
#include <locks/wait.h>

#include <scheduler/schedule.h>

#include <utils/tree.h>
#include <utils/list.h>
#include <utils/string.h>

struct dirent* Vfs_Root;
struct dirent* Vfs_DevRoot;
mount_list Vfs_Mounted;

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
OBOS_STATIC_ASSERT(sizeof(driver_file_perm) == sizeof(file_perm), "Invalid sizes!");
// static vnode* create_vnode(mount* mountpoint, dev_desc desc, file_type* t)
// {
//     file_type type = 0;
//     driver_file_perm perm = {};
//     mountpoint->fs_driver->driver->header.ftable.get_file_perms(desc, &perm);
//     mountpoint->fs_driver->driver->header.ftable.get_file_type(desc, &type);
//     vnode* vn = Vfs_Calloc(1, sizeof(vnode));
//     switch (type)
//     {
//         case FILE_TYPE_REGULAR_FILE:
//             vn->vtype = VNODE_TYPE_REG;
//             mountpoint->fs_driver->driver->header.ftable.get_max_blk_count(desc, &vn->filesize);
//             break;
//         case FILE_TYPE_DIRECTORY:
//             vn->vtype = VNODE_TYPE_DIR;
//             break;
//         case FILE_TYPE_SYMBOLIC_LINK:
//         {
//             vn->vtype = VNODE_TYPE_LNK;        
//             break;
//         }
//         default:
//             OBOS_ASSERT(type);
//     }
//     vn->mount_point = mountpoint;
//     vn->desc = desc;
//     memcpy(&vn->perm, &perm, sizeof(file_perm));
//     if (t)
//         *t = type;
//     return vn;
// }
// static iterate_decision callback(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata)
// {
//     uintptr_t *udata = (uintptr_t*)userdata;
//     mount* mountpoint = (mount*)udata[0];
//     vdev* fs_driver = (vdev*)udata[1];
//     vdev* device = (vdev*)udata[2];
//     OBOS_UNUSED(blkSize);
//     OBOS_UNUSED(device);
//     file_type type = 0;
//     const char* path = nullptr;
//     OBOS_UNUSED(mountpoint);
//     OBOS_UNUSED(desc);
//     OBOS_UNUSED(blkCount);
//     OBOS_UNUSED(device);
//     fs_driver->driver->header.ftable.query_path(desc, &path);
//     fs_driver->driver->header.ftable.get_file_type(desc, &type);
//     size_t pathlen = strlen(path);
//     const char* tok = path;
//     bool is_base = false;
//     size_t tok_len = 0;
//     {
//         size_t currentPathLen = strlen(tok)-1;
//         if (tok[currentPathLen] != '/')
//             currentPathLen++;
//         while (tok[currentPathLen] == '/')
//             currentPathLen--;
//         tok_len = strchr(tok, '/');
//         if (tok_len != currentPathLen)
//             tok_len--;
//         else
//             is_base = true;
//     }
//     dirent* last = mountpoint->root;
//     char* currentPath = Vfs_Calloc(pathlen + 1, sizeof(char));
//     size_t currentPathLen = 0;
//     while (tok < (path+pathlen))
//     {
//         char* token = Vfs_Calloc(tok_len + 1, sizeof(char));
//         memcpy(token, tok, tok_len);
//         token[tok_len] = 0;
//         memcpy(currentPath + currentPathLen, token, tok_len);
//         currentPathLen += tok_len;
//         if (!is_base)
//             currentPath[currentPathLen++] = '/';
//         dirent* new = VfsH_DirentLookupFromCacheOnly(token, last ? last : mountpoint->root);
//         if (new && new->d_parent == mountpoint->root->d_parent)
//             new = nullptr;
//         if (!new)
//         {
//             // Allocate a new dirent.
//             new = Vfs_Calloc(1, sizeof(dirent));
//             OBOS_StringSetAllocator(&new->name, Vfs_Allocator);
//             OBOS_InitStringLen(&new->name, token, tok_len);
//             dev_desc curdesc = 0;
//             file_type curtype = 0;
//             if (is_base)
//                 curdesc = desc;
//             else
//                 fs_driver->driver->header.ftable.path_search(&curdesc, (void*)udata[2], currentPath);
//             mountpoint->fs_driver->driver->header.ftable.get_file_type(desc, &type);
//             vnode* new_vn = create_vnode(mountpoint, curdesc, &curtype);
//             new->vnode = new_vn;
//             new->vnode->refs++;
//             if (curtype == FILE_TYPE_SYMBOLIC_LINK)
//                 mountpoint->fs_driver->driver->header.ftable.get_linked_path(new_vn->desc, &new_vn->un.linked);
//         }
//         if (!new->d_prev_child && !new->d_next_child && last->d_children.head != new)
//             VfsH_DirentAppendChild(last ? last : mountpoint->root, new);
//         last = new;
//         Vfs_Free(token);

//         tok += str_search(tok, '/');
//         size_t currentPathLen = strlen(tok)-1;
//         if (currentPathLen == (size_t)-1)
//             currentPathLen = 0;
//         if (tok[currentPathLen] != '/')
//             currentPathLen++;
//         while (tok[currentPathLen] == '/')
//             currentPathLen--;
//         tok_len = strchr(tok, '/');
//         if (tok_len != currentPathLen)
//             tok_len--;
//         else
//             is_base = true;
//     }
//     if (type == FILE_TYPE_DIRECTORY)
//         fs_driver->driver->header.ftable.list_dir(desc, (void*)udata[2], callback, udata);
//     return ITERATE_DECISION_CONTINUE;
// }
obos_status Vfs_Mount(const char* at_, vnode* on, vdev* fs_driver, mount** pMountpoint)
{
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
    if (at->d_children.nChildren)
        return OBOS_STATUS_INVALID_ARGUMENT;
    mount* mountpoint = Vfs_Calloc(1, sizeof(mount));
    if (pMountpoint)
        *pMountpoint = mountpoint;
    mountpoint->mounted_on = at->vnode;
    at->vnode->un.mounted = mountpoint;
    // at->vnode->mount_point = mountpoint;
    at->vnode->flags |= VFLAGS_MOUNTPOINT;
    mountpoint->root = at;
    // uintptr_t udata[3] = {
    //     (uintptr_t)mountpoint,
    //     (uintptr_t)fs_driver,
    //     (uintptr_t)on,
    // };
    mountpoint->fs_driver = memcpy(Vfs_Calloc(1, sizeof(vdev)), fs_driver, sizeof(*fs_driver));
    if (mountpoint->device)
        mountpoint->device->refs++;
    mountpoint->device = on;
    if (fs_driver->driver->header.ftable.mount)
    {
        obos_status status = fs_driver->driver->header.ftable.mount(on, at);
        if (obos_is_success(status))
            LIST_APPEND(mount_list, &Vfs_Mounted, mountpoint);
        return status;
    }
    // else
    //     fs_driver->driver->header.ftable.list_dir(UINTPTR_MAX, on, callback, udata);
    LIST_APPEND(mount_list, &Vfs_Mounted, mountpoint);
    return OBOS_STATUS_SUCCESS;
}
bool VfsH_LockMountpoint(mount* point)
{
    if (!point)
        return true;
    point->nWaiting++;
    obos_status status = Core_MutexAcquire(&point->lock);
    if (obos_is_error(status) && status != OBOS_STATUS_ABORTED)
        return false;
    if (status == OBOS_STATUS_ABORTED)
    {
        if (point->awaitingFree && !(--point->nWaiting))
            Vfs_Free(point);
        return false;
    }
    point->nWaiting--;
    return true;
}
bool VfsH_UnlockMountpoint(mount* point)
{
    if (!point)
        return true;
    return obos_is_success(Core_MutexRelease(&point->lock));
}
static bool deref_vnode(vnode* vn)
{
    if (!(--vn->refs))
    {
        if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO)
            if (!(vn->un.device->refs--))
                Vfs_Free(vn->un.device);
        Vfs_Free(vn);
        return true;
    }
    return false;
}
static void close_fd(fd* desc)
{
    desc->flags &= ~FD_FLAGS_OPEN;
    LIST_REMOVE(fd_list, &desc->vn->opened, desc);
    deref_vnode(desc->vn);
    
}
static void foreach_dirent(mount *what, void(*cb)(mount* what, dirent* ent, void* userdata), void* userdata)
{
    for (dirent* curr = LIST_GET_HEAD(dirent_list, &what->dirent_list); curr; )
    {
        dirent* next = LIST_GET_NEXT(dirent_list, &what->dirent_list, curr);
        cb(what, curr, userdata);
        curr = next;
    }
}
// Closes FDs and waits for pending async IO operations to finish.
static void stage_one(mount* unused, dirent* ent, void* userdata)
{
    OBOS_UNUSED(unused);
    OBOS_UNUSED(userdata);
    for (fd* curr = LIST_GET_HEAD(fd_list, &ent->vnode->opened); curr; )
    {
        fd* next = LIST_GET_NEXT(fd_list, &ent->vnode->opened, curr);
        close_fd(curr);
        curr = next;
    }
}
static void stage_two(mount* unused, dirent* ent, void* userdata)
{
    OBOS_UNUSED(unused);
    OBOS_UNUSED(userdata);
    if (ent == Vfs_DevRoot || ent->d_parent == Vfs_DevRoot || ent == Vfs_Root)
        return; // Don't free this.
    bool vnode_freed = deref_vnode(ent->vnode);
    // if (ent->d_parent)
    // {
    //     if (deref_vnode(ent->d_parent->vnode))
    //     {
    //         OBOS_FreeString(&ent->name);
    //         Vfs_Free(ent);    
    //     }
    // }
    if (!vnode_freed)
        return;
    OBOS_FreeString(&ent->name);
    Vfs_Free(ent);
}
obos_status Vfs_Unmount(mount* what)
{
    if (!what)
        return OBOS_STATUS_INVALID_ARGUMENT;

    // Sync pages.
    Mm_PageWriterOperation = PAGE_WRITER_SYNC_FILE;
    Mm_WakePageWriter(true);
    Mm_WakePageWriter(true);

    Core_MutexAcquire(&what->lock);
    what->mounted_on->un.mounted = nullptr;
    what->mounted_on->flags &= ~VFLAGS_MOUNTPOINT;
    foreach_dirent(what, stage_one, nullptr);
    namecache_ent* curr;
    for (curr = RB_MIN(namecache, &what->nc); curr;)
    {
        namecache_ent* next = RB_RIGHT(curr, rb_cache);
        deref_vnode(curr->ref);
        OBOS_FreeString(&curr->path);
        Vfs_Free(curr);
        curr = next;
    }
    what->root->d_children.head = nullptr;
    what->root->d_children.tail = nullptr;
    what->root->d_children.nChildren = 0;
    foreach_dirent(what, stage_two, nullptr);
    LIST_REMOVE(mount_list, &Vfs_Mounted, what);
    if (what->root == Vfs_Root)
    {
        Vfs_Root->vnode->mount_point = nullptr;
        Vfs_Root->vnode->un.mounted = nullptr;
    }
    what->awaitingFree = true;
    if (!what->nWaiting)
        Vfs_Free(what);
    else // the last thread to be waken up will free the mount point.
    {
        what->lock.ignoreAllAndBlowUp = true;
        CoreH_SignalWaitingThreads(WAITABLE_OBJECT(what->lock), true, false);
    }
    return OBOS_STATUS_SUCCESS;
}
obos_status Vfs_UnmountP(const char* at)
{
    dirent* resolved = VfsH_DirentLookup(at);
    if (!resolved)
        return OBOS_STATUS_NOT_FOUND;
    if (~resolved->vnode->flags & VFLAGS_MOUNTPOINT)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return Vfs_Unmount(resolved->vnode->un.mounted);
}

RB_GENERATE(namecache, namecache_ent, rb_cache, cmp_namecache_ent);
LIST_GENERATE(mount_list, mount, node);

obos_status Vfs_StatFSInfo(struct vnode* vn, struct drv_fs_info* out)
{
    if (!vn)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (~vn->flags & VFLAGS_MOUNTPOINT || !out)
        return OBOS_STATUS_INVALID_ARGUMENT;
    return vn->un.mounted->fs_driver->driver->header.ftable.stat_fs_info(vn->un.mounted->device, out);
}
