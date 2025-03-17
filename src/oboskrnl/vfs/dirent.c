/*
 * oboskrnl/vfs/dirent.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <syscall.h>

#include <vfs/dirent.h>
#include <vfs/alloc.h>
#include <vfs/mount.h>
#include <vfs/vnode.h>
#include <vfs/namecache.h>
#include <vfs/limits.h>

#include <allocators/base.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <utils/string.h>
#include <utils/list.h>
#include <utils/tree.h>

// TODO: Make namecache a tree of cached directory hierarchy, instead of storing paths in the cache

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
    OBOS_StringSetAllocator(&what.path, Vfs_Allocator);
    OBOS_InitString(&what.path, path);
    namecache_ent* hit = RB_FIND(namecache, nc, &what);
    OBOS_FreeString(&what.path);
    return hit;
}
static dirent* namecache_lookup(namecache* nc, const char* path)
{
    return nullptr;
    namecache_ent* nc_ent = namecache_lookup_internal(nc, path);
    if (!nc_ent)
        return nullptr;
    return nc_ent->ent;
}
static void namecache_insert(namecache* nc, dirent* what, const char* path, size_t pathlen)
{
    return;
    namecache_ent* ent = Vfs_Calloc(1, sizeof(namecache_ent));
    ent->ent = what;
    ent->ref = what->vnode;
    ent->ref->refs++;
    OBOS_StringSetAllocator(&ent->path, Vfs_Allocator);
    OBOS_InitStringLen(&ent->path, path, pathlen);
    if (!namecache_lookup_internal(nc, OBOS_GetStringCPtr(&ent->path)))
        RB_INSERT(namecache, nc, ent);
    else
    {
        OBOS_FreeString(&ent->path);
        Vfs_Free(ent);
    }
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
        {
            size_t currentPathLen = strlen(*path+(*lastMountPoint))-1;
            if ((*path+(*lastMountPoint))[currentPathLen] != '/')
                currentPathLen++;
            while ((*path+(*lastMountPoint))[currentPathLen] == '/')
                currentPathLen--;
            namecache_insert(&(*lastMount)->nc, curr, *path+(*lastMountPoint), currentPathLen);
        }
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
    while ((*tok)[(*tok_len) - 1] == '/')
        (*tok_len)--;
    if (curr->vnode && curr->vnode->flags & VFLAGS_MOUNTPOINT)
    {
        (*lastMountPoint) = ((*tok)-(*path));
        (*lastMount) = curr->vnode->un.mounted;
        dirent* hit = namecache_lookup(&curr->vnode->un.mounted->nc, *tok);
        if (!hit)
            *root = curr->vnode->un.mounted->root;
        return hit;
    }
    return nullptr;
}
dirent* VfsH_DirentLookupFrom(const char* path, dirent* root_par)
{
    if (!path)
        return nullptr;
    if (path[0] == 0)
        return root_par;
    dirent* root = root_par;
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
    // Offset of the last mount point in the path.
    size_t lastMountPoint = 0;
    mount* lastMount = root->vnode->flags & VFLAGS_MOUNTPOINT ? root->vnode->un.mounted : root->vnode->mount_point;
    if (root->vnode && root->vnode->flags & VFLAGS_MOUNTPOINT)
    {
        // If 'root' is at the root of it's mount point, consult the name cache.
        dirent* hit = namecache_lookup(&root->vnode->un.mounted->nc, path);
        if (hit)
            return hit;
    }
    while(root)
    {
        dirent* curr = root;
        if (tok[0] == '.')
        {
            if (tok[1] == '.')
                root = root->d_parent;
            else if (tok_len == 1)
                OBOSS_SpinlockHint(); // this token is just a '.', so we need to ignore the next else if.
            else if (tok[1] != 0)
                goto down;
            const char *newtok = tok + str_search(tok, '/');
            tok = newtok;
            size_t currentPathLen = strlen(tok)-1;
            if (tok[currentPathLen] != '/')
                currentPathLen++;
            while (tok[currentPathLen] == '/')
                currentPathLen--;
            tok_len = strchr(tok, '/');
            if (tok_len != currentPathLen)
                tok_len--;
            while (tok[tok_len - 1] == '/')
                tok_len--;
            continue;
        }
        down:
        if (!tok_len)
            return root;
        if (OBOS_CompareStringNC(&root->name, tok, tok_len))
        {
            // Match!
            dirent* what = 
                on_match(&curr, &root, &tok, &tok_len, &path, &path_len, &lastMountPoint, &lastMount);
            root = curr->d_children.head;
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
                // else
                //     return nullptr;
                curr = curr->d_children.head ? curr->d_children.head : curr;
                break;
            }

            // root = curr->d_children.head ? curr->d_children.head : root;
            curr = curr->d_next_child;
        }
        if (!curr)
        {
            root = root->d_parent;
            if (root == root_par->d_parent)
                break;
        }
    }
    return nullptr;
}
dirent* VfsH_DirentLookup(const char* path)
{
    dirent* begin = Core_GetCurrentThread()->proc->cwd;
    if (!begin)
        begin = Vfs_Root;
    if (path[0] == 0)
        return begin;
    if (strcmp(path, "/"))
        return Vfs_Root;
    if (path[0] == '/')
        begin = Vfs_Root;
    return VfsH_DirentLookupFrom(path, begin);
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
    child->d_parent = parent;
    mount* const point = parent->vnode->mount_point ? parent->vnode->mount_point : parent->vnode->un.mounted;
    LIST_APPEND(dirent_list, &point->dirent_list, child);
    if (child->vnode)
        child->vnode->refs++;
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
    what->d_parent = nullptr; // we're now an orphan :(
    mount* const point = parent->vnode->mount_point ? parent->vnode->mount_point : parent->vnode->un.mounted;
    LIST_REMOVE(dirent_list, &point->dirent_list, what);
}

vnode* Drv_AllocateVNode(driver_id* drv, dev_desc desc, size_t filesize, vdev** dev_p, uint32_t type)
{
    static file_perm default_fileperm = {
        .owner_read = true,
        .group_read = true,
        .other_read = false,
        .owner_write = true,
        .group_write = true,
        .other_write = false,
        .owner_exec = false,
        .group_exec = false,
        .other_exec = false,
    };
    // It is legal to call this from the kernel
    // if (!drv)
    //     return nullptr;
    vdev* dev = nullptr;
    if (drv)
    {
        dev = Vfs_Calloc(1, sizeof(vdev));
        dev->desc = desc;
        dev->driver = drv;
        dev->refs++;
    }
    vnode *vn = Vfs_Calloc(1, sizeof(vnode));
    vn->desc = desc;
    vn->filesize = filesize;
    vn->un.device = dev;
    vn->perm = default_fileperm;
    vn->vtype = type;
    vn->group_uid = ROOT_GID;
    vn->owner_uid = ROOT_UID;
    if (dev_p)
        *dev_p = dev;
    return vn;    
}
dirent* Drv_RegisterVNode(struct vnode* vn, const char* const dev_name)
{
    if (!vn || !dev_name)
        return nullptr;
    dirent* parent = VfsH_DirentLookup(OBOS_DEV_PREFIX);
    if (!parent)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Could not find directory at OBOS_DEV_PREFIX (%s) specified at build time.\n", __func__, OBOS_DEV_PREFIX);
    dirent* ent = VfsH_DirentLookupFrom(dev_name, parent);
    if (ent && ent->vnode == vn)
        return ent;
    mount* const point = parent->vnode->mount_point ? parent->vnode->mount_point : parent->vnode->un.mounted;
    if (!ent)
        ent = Vfs_Calloc(1, sizeof(dirent));
    else
    {
        ent->vnode = vn;

        ent->vnode->mount_point = point;
        return ent;
    }
    if (!VfsH_LockMountpoint(point))
        return nullptr;
    ent->vnode = vn;
    ent->vnode->mount_point = point;
    OBOS_StringSetAllocator(&ent->name, Vfs_Allocator);
    OBOS_InitString(&ent->name, dev_name);
    VfsH_DirentAppendChild(parent, ent);
    VfsH_UnlockMountpoint(point);
    return ent;
}
LIST_GENERATE(dirent_list, dirent, node);

struct mlibc_dirent {
    uint32_t d_ino;
    off_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[MAX_FILENAME_LEN];
};

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
// #define DT_WHT 14

obos_status Vfs_ReadEntries(dirent* dent, void* buffer, size_t szBuf, dirent** last, size_t* nRead)
{
    if (!VfsH_LockMountpoint(dent->vnode->mount_point))
        return OBOS_STATUS_ABORTED;
    if (!buffer)
        return OBOS_STATUS_INVALID_ARGUMENT;
    size_t nDirentsToRead = szBuf/sizeof(struct mlibc_dirent);
    if (!nDirentsToRead)
        return OBOS_STATUS_INVALID_ARGUMENT;

    size_t nReadableDirents = 0;
    struct mlibc_dirent* iter = buffer;
    for (dirent* curr = dent; curr && nReadableDirents < nDirentsToRead; nReadableDirents++)
    {
        // TODO: Directory inodes (maybe?)
        iter->d_ino = UINT32_MAX;
        iter->d_off = 0;
        iter->d_reclen = sizeof(struct mlibc_dirent);

        switch (curr->vnode->vtype) {
            case VNODE_TYPE_REG:
                iter->d_type = DT_REG;
                break;
            case VNODE_TYPE_BLK:
                iter->d_type = DT_BLK;
                break;
            case VNODE_TYPE_CHR:
                iter->d_type = DT_CHR;
                break;
            case VNODE_TYPE_LNK:
                iter->d_type = DT_LNK;
                break;
            case VNODE_TYPE_FIFO:
                iter->d_type = DT_FIFO;
                break;
            case VNODE_TYPE_SOCK:
                iter->d_type = DT_SOCK;
                break;
            case VNODE_TYPE_DIR:
                iter->d_type = DT_DIR;
                break;
            default:
                iter->d_type = DT_UNKNOWN;
                break;
        }

        memcpy(iter->d_name, OBOS_GetStringCPtr(&curr->name), OBOS_MIN(OBOS_GetStringSize(&curr->name), MAX_FILENAME_LEN));

        iter++;
        curr = curr->d_next_child;
        *last = curr;
    }

    if (nRead)
        *nRead = nReadableDirents*sizeof(struct mlibc_dirent);

    VfsH_UnlockMountpoint(dent->vnode->mount_point);
    return nReadableDirents ? OBOS_STATUS_SUCCESS : OBOS_STATUS_EOF;
}

static char* realpath(dirent* ent)
{
    size_t path_len = 0;
    char* path = nullptr;

    // Calculate path_len.
    for (dirent* curr = ent; curr != Vfs_Root; )
    {
        path_len += OBOS_GetStringSize(&curr->name) + 1;
        curr = curr->d_parent;
    }

    path = Vfs_Malloc(path_len+1);
    path[path_len] = 0;

    size_t left = path_len;
    dirent* curr = ent;
    while (left && curr)
    {
        memcpy(&path[left-OBOS_GetStringSize(&curr->name)], OBOS_GetStringCPtr(&curr->name), OBOS_GetStringSize(&curr->name));

        left -= OBOS_GetStringSize(&curr->name);
        path[left-1] = '/';
        left -= 1;
        curr = curr->d_parent;
    }

    return path;
}

obos_status VfsH_Chdir(void* target_, const char *path)
{
    if (!target_)
        return OBOS_STATUS_INVALID_ARGUMENT;

    process* target = target_;
    dirent* ent = VfsH_DirentLookup(path);
    if (!ent)
        return OBOS_STATUS_NOT_FOUND;
    
    Vfs_Free((char*)target->cwd_str);
    
    target->cwd = ent;
    target->cwd_str = realpath(ent);
    
    return OBOS_STATUS_SUCCESS;
}
obos_status VfsH_ChdirEnt(void* /* struct process */ target_, dirent* ent)
{
    process* target = target_;
    if (!ent || !target)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    Vfs_Free((char*)target->cwd_str);

    target->cwd = ent;
    target->cwd_str = realpath(ent);
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_GetCWD(char* path, size_t len)
{
    process* target = Core_GetCurrentThread()->proc;
    if (!path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (len < strlen(target->cwd_str))
        return OBOS_STATUS_NO_SPACE;
    memcpy_k_to_usr(path, target->cwd, strlen(target->cwd_str));
    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_Chdir(const char *upath)
{
    char* path = nullptr;
    size_t sz_path = 0;
    obos_status status = OBOSH_ReadUserString(upath, nullptr, &sz_path);
    if (obos_is_error(status))
        return status;
    path = ZeroAllocate(OBOS_KernelAllocator, sz_path+1, sizeof(char), nullptr);
    OBOSH_ReadUserString(upath, path, nullptr);

    status = VfsH_Chdir(Core_GetCurrentThread()->proc, path);
    Free(OBOS_KernelAllocator, path, sz_path);

    return status;
}

obos_status Sys_ChdirEnt(handle desc)
{
    OBOS_LockHandleTable(OBOS_CurrentHandleTable());
    obos_status status = OBOS_STATUS_SUCCESS;
    handle_desc* dent = OBOS_HandleLookup(OBOS_CurrentHandleTable(), desc, HANDLE_TYPE_DIRENT, false, &status);
    if (!dent)
    {
        OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());
        return status;
    }
    OBOS_UnlockHandleTable(OBOS_CurrentHandleTable());

    return VfsH_ChdirEnt(Core_GetCurrentThread()->proc, dent->un.dirent);
}