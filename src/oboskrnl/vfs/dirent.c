/*
 * oboskrnl/vfs/dirent.c
 *
 * Copyright (c) 2024-2026 Omar Berrow
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
#include <vfs/limits.h>

#include <allocators/base.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <mm/alloc.h>

#include <utils/string.h>
#include <utils/list.h>
#include <utils/tree.h>

#include <driver_interface/header.h>

static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}
static vnode* create_vnode(mount* mountpoint, dev_desc desc, file_type* t)
{
    if (mountpoint->fs_driver->driver->header.ftable.vnode_search)
    {
        vnode* vn = nullptr;
        obos_status status = mountpoint->fs_driver->driver->header.ftable.vnode_search((void**)&vn, desc, mountpoint->fs_driver);
        if (obos_is_success(status))
        {
            OBOS_ENSURE(vn);
            vn->mount_point = mountpoint;
            if (t)
            {
                switch (vn->vtype) {
                    case VNODE_TYPE_LNK:
                        *t = FILE_TYPE_SYMBOLIC_LINK;
                        break;
                    case VNODE_TYPE_REG:
                        *t = FILE_TYPE_REGULAR_FILE;
                        break;
                    case VNODE_TYPE_DIR:
                        *t = FILE_TYPE_DIRECTORY;
                        break;
                    default: OBOS_UNREACHABLE;
                }
            }
            return vn;
        }
    }
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
            vn->vtype = VNODE_TYPE_LNK;        
            break;
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
        }
        thread* cur_thr = Core_GetCurrentThread();
        if (curr->flags & DIRENT_REFERS_CTTY)
        {
            if (!(cur_thr && cur_thr->proc && cur_thr->proc->session && cur_thr->proc->session->controlling_tty))
                return nullptr;
            return cur_thr->proc->session->controlling_tty->ent;
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
    }
    return nullptr;
}

static dirent* lookup(const char* path, dirent* root_par, bool only_cache)
{
    if (root_par->vnode->vtype != VNODE_TYPE_DIR)
        return nullptr;
    // for (volatile bool b = true; b;)
    //     ;
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
    if (!root->vnode)
        return nullptr;
    // Offset of the last mount point in the path.
    size_t lastMountPoint = 0;
    mount* lastMount = root->vnode->flags & VFLAGS_MOUNTPOINT ? root->vnode->un.mounted : root->vnode->mount_point;
    while(root)
    {
        dirent* curr = root;
        if (curr->vnode->vtype == VNODE_TYPE_LNK)
        {
            // If the linked vnode is a directory, set curr to the linked directory
            dirent* ent = VfsH_FollowLink(curr);
            if (!ent)
                return nullptr; // broken link :(
            if (ent->vnode->vtype == VNODE_TYPE_DIR)
                root = curr = ent;
        }
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
            break;
    }

    if (only_cache || !lastMount->fs_driver->driver->header.ftable.path_search)
        return nullptr;

    // Not in the dirent tree cache
    // Start looking for each path component 
    // until we get to the end of the string
    // using path_search
    size_t path_mnt_len = (path_len-lastMountPoint);
    char* path_mnt = memcpy(Vfs_Malloc((path_len-lastMountPoint)+1), path+lastMountPoint, (path_len-lastMountPoint)+1);
    bool is_base = false;
    tok = path_mnt;
    tok_len = 0;
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
    char* currentPath = Vfs_Calloc(path_mnt_len + 1, sizeof(char));
    size_t currentPathLen = 0;
    vdev* fs_driver = lastMount->fs_driver;
    mount* mountpoint = lastMount;
    dirent* last = root_par;
    while (tok < (path_mnt+path_mnt_len))
    {
        char* token = Vfs_Calloc(tok_len + 1, sizeof(char));
        memcpy(token, tok, tok_len);
        token[tok_len] = 0;
        memcpy(currentPath + currentPathLen, token, tok_len);
        currentPathLen += tok_len;
        if (!is_base)
            currentPath[currentPathLen++] = '/';
        // printf("%.*s\n", currentPathLen, currentPath);
        dirent* new = VfsH_DirentLookupFromCacheOnly(token, last ? last : mountpoint->root);
        new = VfsH_FollowLink(new);
        if (new && new->d_parent == mountpoint->root->d_parent)
            new = nullptr;
        if (!new)
        {
            dev_desc curdesc = 0;
            file_type curtype = 0;
            obos_status status = fs_driver->driver->header.ftable.path_search(&curdesc, mountpoint->device, token, last->vnode->desc);
            if (obos_is_error(status))
            {
                Vfs_Free(token);
                Vfs_Free(path_mnt);
                Vfs_Free(currentPath);
                return nullptr;
            }
            // Allocate a new dirent.
            new = Vfs_Calloc(1, sizeof(dirent));
            OBOS_StringSetAllocator(&new->name, Vfs_Allocator);
            OBOS_InitStringLen(&new->name, token, tok_len);
            // mountpoint->fs_driver->driver->header.ftable.get_file_type(desc, &type);
            vnode* new_vn = create_vnode(mountpoint, curdesc, &curtype);
            new->vnode = new_vn;
            new->vnode->refs++;
            if (curtype == FILE_TYPE_SYMBOLIC_LINK && !new_vn->un.linked)
                mountpoint->fs_driver->driver->header.ftable.get_linked_path(new_vn->desc, &new_vn->un.linked);
        }
        if (!new->d_prev_child && !new->d_next_child && last->d_children.head != new && last != new)
            VfsH_DirentAppendChild(last ? last : mountpoint->root, new);
        last = new;
        Vfs_Free(token);
        if (last->vnode->vtype == VNODE_TYPE_LNK)
        {
            last = VfsH_FollowLink(last);
            if (!last)
                break;
            mountpoint = last->vnode->flags & VFLAGS_MOUNTPOINT ? last->vnode->un.mounted : last->vnode->mount_point;
            fs_driver = mountpoint->fs_driver;
            currentPath = VfsH_DirentPath(last, mountpoint->root);
            currentPathLen = strlen(currentPath);
            if (currentPath[currentPathLen-1] != '/')
            {
                currentPath = Vfs_Realloc(currentPath, currentPathLen+1+1/*nul terminator*/);
                currentPath[currentPathLen] = '/';
                currentPath[++currentPathLen] = 0;
            }
        }
        else
        {
            mountpoint = last->vnode->flags & VFLAGS_MOUNTPOINT ? last->vnode->un.mounted : last->vnode->mount_point;
            fs_driver = mountpoint->fs_driver;
        }

        tok += str_search(tok, '/');
        size_t currentPathLen2 = strlen(tok)-1;
        if (currentPathLen2 == (size_t)-1)
            currentPathLen2 = 0;
        if (tok[currentPathLen2] != '/')
            currentPathLen2++;
        while (tok[currentPathLen2] == '/')
            currentPathLen2--;
        tok_len = strchr(tok, '/');
        if (tok_len != currentPathLen2)
            tok_len--;
        else
            is_base = true;
    }

    Vfs_Free(path_mnt);
    Vfs_Free(currentPath);
    return last;
}

dirent* VfsH_DirentLookupFromCacheOnly(const char* path, dirent* root_par)
{
    if (!root_par)
        return nullptr;
    return lookup(path, root_par, true);
}
dirent* VfsH_DirentLookupFrom(const char* path, dirent* root_par)
{
    if (!root_par)
        return nullptr;
    return lookup(path, root_par, false);
}

dirent* VfsH_DirentLookupWD(const char* path, dirent* wd)
{
    dirent* begin = wd;
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

dirent* VfsH_DirentLookup(const char* path)
{
    dirent* cwd = nullptr;
    if (Core_GetCurrentThread() && Core_GetCurrentThread()->proc)
        cwd = Core_GetCurrentThread()->proc->cwd;
    return VfsH_DirentLookupWD(path, cwd);
}

dirent* VfsH_FollowLink(dirent* ent)
{
    if (!ent)
        return nullptr;
    while (ent && ent->vnode->vtype == VNODE_TYPE_LNK)
        ent = VfsH_DirentLookupWD(ent->vnode->un.linked, ent->d_parent ? ent->d_parent : Vfs_Root);
    return ent;
}

void VfsH_DirentAppendChild(dirent* parent, dirent* child)
{
    OBOS_ENSURE(parent != child);
    OBOS_ENSURE(parent);
    OBOS_ENSURE(child);
    if(!parent->d_children.head)
        parent->d_children.head = child;
    if (parent->d_children.tail)
        parent->d_children.tail->d_next_child = child;
    child->d_prev_child = parent->d_children.tail;
    parent->d_children.tail = child;
    parent->d_children.nChildren++;
    child->d_parent = parent;
    mount* const point = parent->vnode->mount_point ? parent->vnode->mount_point : parent->vnode->un.mounted;
    if (point)
        LIST_APPEND(dirent_list, &point->dirent_list, child);
    if (child->vnode)
        child->vnode->refs++;
}
void VfsH_DirentRemoveChild(dirent* parent, dirent* what)
{
    OBOS_ENSURE(parent);
    OBOS_ENSURE(what);
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

#ifdef __x86_64__
#   include <arch/x86_64/cmos.h>
#endif

static long get_current_time()
{
    long current_time = 0;
#ifdef __x86_64__
    Arch_CMOSGetEpochTime(&current_time);
#endif
    return current_time;
}

static uint32_t devfs_inode = 3;

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
    vn->inode = devfs_inode++;
    vn->perm = default_fileperm;
    vn->vtype = type;
    vn->gid = ROOT_GID;
    vn->uid = ROOT_UID;
    vn->times.access = get_current_time();
    vn->times.birth = vn->times.access;
    vn->times.change = vn->times.access;
    if (dev_p)
        *dev_p = dev;
    return vn;    
}
dirent* Drv_RegisterVNode(struct vnode* vn, const char* const dev_name)
{
    return Drv_RegisterVNodeEx(vn, dev_name, 0);
}

dirent* Drv_RegisterVNodeEx(struct vnode* vn, const char* const dev_name, int flags)
{
    if (!vn || !dev_name)
        return nullptr;
    dirent* parent = Vfs_DevRoot;
    if (flags & REGISTER_VNODE_IS_PTY)
        parent = VfsH_DirentLookupFrom("pts", parent);
    if (!parent)
        return nullptr;
    
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

LIST_GENERATE_INTERNAL(dirent_list, dirent, node, OBOS_EXPORT);

static iterate_decision populate_cb(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name)
{
    OBOS_UNUSED(blkSize && blkCount);
    dirent* const dent = userdata;
    for (dirent* child = dent->d_children.head; child; )
    {
        if (OBOS_CompareStringC(&child->name, name))
            return ITERATE_DECISION_CONTINUE;

        child = child->d_next_child;
    }

    mount* point = Vfs_GetVnodeMount(dent->vnode);
    vnode* vn = create_vnode(point, desc, nullptr);
    dirent* new = Vfs_Calloc(1, sizeof(dirent));
    OBOS_StringSetAllocator(&new->name, Vfs_Allocator);
    OBOS_InitString(&new->name, name);
    new->vnode = vn;
    VfsH_DirentAppendChild(dent, new);
    LIST_APPEND(dirent_list, &point->dirent_list, new);
    return ITERATE_DECISION_CONTINUE;
}

void Vfs_PopulateDirectory(dirent* dent)
{
    mount* point = Vfs_GetVnodeMount(dent->vnode);
    driver_header* driver = Vfs_GetVnodeDriver(dent->vnode);
    if (dent->vnode->vtype != VNODE_TYPE_DIR)
        return;
    if (driver->ftable.list_dir)
    {
        obos_status status = driver->ftable.list_dir(dent->vnode->flags & VFLAGS_MOUNTPOINT ? UINTPTR_MAX : dent->vnode->desc, point->device, populate_cb, dent);
        if (obos_is_error(status))
            OBOS_Error("list_dir returned %d!\n", status);
    }
    else
        OBOS_Error("driver->ftable.list_dir == nullptr!\n");
}

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

char* VfsH_DirentPath(dirent* ent, dirent* relative_to)
{
    if (!relative_to)
        relative_to = Vfs_Root;
    if (!ent)
        return nullptr;
    
    size_t path_len = 0;
    char* path = nullptr;

    // Calculate path_len.
    for (dirent* curr = ent; curr != relative_to; )
    {
        path_len += OBOS_GetStringSize(&curr->name) + 1;
        curr = curr->d_parent;
    }

    path = Vfs_Malloc(path_len+1);
    path[path_len] = 0;

    size_t left = path_len;
    dirent* curr = ent;
    while (left && (relative_to == Vfs_Root ? (!!curr) : (relative_to != curr)))
    {
        memcpy(&path[left-OBOS_GetStringSize(&curr->name)], OBOS_GetStringCPtr(&curr->name), OBOS_GetStringSize(&curr->name));

        left -= OBOS_GetStringSize(&curr->name);
        path[left-1] = '/';
        left -= 1;
        curr = curr->d_parent;
    }

    return path;
}

static char* strcpy(const char* str)
{
    size_t len = strlen(str);
    return memcpy(Allocate(OBOS_KernelAllocator, len+1, nullptr), str, len+1);
}

char* VfsH_DirentPathKAlloc(dirent* ent, dirent* relative_to)
{
    char* res = VfsH_DirentPath(ent, relative_to);
    if (!res)
        return nullptr;
    char* ret = strcpy(res);
    Vfs_Free(res);
    return ret;
}

static bool check_chdir_perms(dirent* ent)
{
    return Vfs_Access(ent->vnode, false, false, true) == OBOS_STATUS_SUCCESS;
}

obos_status VfsH_Chdir(void* target_, const char *path)
{
    if (!target_)
        return OBOS_STATUS_INVALID_ARGUMENT;

    process* target = target_;
    dirent* ent = VfsH_DirentLookup(path);
    if (!ent || !ent->vnode)
        return OBOS_STATUS_NOT_FOUND;
    if (ent->vnode->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    if (!check_chdir_perms(ent))
        return OBOS_STATUS_ACCESS_DENIED;
    
    Vfs_Free((char*)target->cwd_str);
    
    target->cwd = ent;
    target->cwd_str = VfsH_DirentPath(ent, nullptr);
    
    return OBOS_STATUS_SUCCESS;
}
obos_status VfsH_ChdirEnt(void* /* struct process */ target_, dirent* ent)
{
    process* target = target_;
    if (!ent || !ent->vnode || !target)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (ent->vnode->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
   
    if (!check_chdir_perms(ent))
        return OBOS_STATUS_ACCESS_DENIED;

    Vfs_Free((char*)target->cwd_str);

    target->cwd = ent;
    target->cwd_str = VfsH_DirentPath(ent, nullptr);
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Sys_GetCWD(char* upath, size_t len)
{
    process* target = Core_GetCurrentThread()->proc;
    if (!upath)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (len < strlen(target->cwd_str))
        return OBOS_STATUS_NO_SPACE;
    obos_status status = OBOS_STATUS_SUCCESS;
    size_t cwd_len = strlen(target->cwd_str);
    
    char* path = Mm_MapViewOfUserMemory(Core_GetCurrentThread()->proc->ctx, upath, nullptr, cwd_len, 0, true, &status);
    if (obos_is_error(status))
        return status;

    memcpy(path, target->cwd_str, cwd_len);
    path[cwd_len] = 0;

    Mm_VirtualMemoryFree(&Mm_KernelContext, path, cwd_len);
    
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
    Free(OBOS_KernelAllocator, path, sz_path+1);

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

    return VfsH_ChdirEnt(Core_GetCurrentThread()->proc, dent->un.dirent->curr);
}

driver_header* Vfs_GetVnodeDriver(vnode* vn)
{
    if (vn->flags & (VFLAGS_EVENT_DEV|VFLAGS_DRIVER_DEAD))
        return nullptr;
    mount* point = Vfs_GetVnodeMount(vn);
    if (!point && vn->vtype != VNODE_TYPE_SOCK && vn->vtype != VNODE_TYPE_FIFO)
        return nullptr;
    if ((vn->vtype == VNODE_TYPE_REG || vn->vtype == VNODE_TYPE_DIR) && !point->fs_driver->driver)
        return nullptr;
    driver_header* driver = (vn->vtype == VNODE_TYPE_REG || vn->vtype == VNODE_TYPE_DIR) ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO  || vn->vtype == VNODE_TYPE_SOCK)
    {
        point = nullptr;
        if (!vn->un.device)
            return nullptr;
        if (!vn->un.device->driver)
            return nullptr;
        driver = &vn->un.device->driver->header;
    }
    return driver;
}

driver_header* Vfs_GetVnodeDriverStat(vnode* vn)
{
    if (vn->flags & (VFLAGS_EVENT_DEV|VFLAGS_DRIVER_DEAD))
        return nullptr;
    mount* point = Vfs_GetVnodeMount(vn);
    if (!point && vn->vtype != VNODE_TYPE_SOCK && vn->vtype != VNODE_TYPE_FIFO)
        return nullptr;
    if ((vn->vtype == VNODE_TYPE_REG || vn->vtype == VNODE_TYPE_DIR) && !point->fs_driver->driver)
        return nullptr;
    driver_header* driver = (vn->vtype == VNODE_TYPE_REG || vn->vtype == VNODE_TYPE_DIR || vn->vtype == VNODE_TYPE_LNK) ? &point->fs_driver->driver->header : nullptr;
    if (vn->vtype == VNODE_TYPE_CHR || vn->vtype == VNODE_TYPE_BLK || vn->vtype == VNODE_TYPE_FIFO || vn->vtype == VNODE_TYPE_SOCK)
    {
        point = nullptr;
        if (!vn->un.device)
            return nullptr;
        driver = &vn->un.device->driver->header;
    }
    return driver;
}
mount* Vfs_GetVnodeMount(vnode* vn)
{
    if (vn->flags & (VFLAGS_EVENT_DEV|VFLAGS_DRIVER_DEAD))
        return nullptr;
    if (vn->vtype == VNODE_TYPE_FIFO || vn->vtype == VNODE_TYPE_SOCK)
        return nullptr;
    return vn->flags & VFLAGS_MOUNTPOINT ? vn->un.mounted : vn->mount_point;
}

obos_status Vfs_Access(vnode* vn, bool read, bool write, bool exec)
{
    uid euid = Core_GetCurrentThread() && Core_GetCurrentThread()->proc ? Core_GetCurrentThread()->proc->euid : 0;
    gid egid = Core_GetCurrentThread() && Core_GetCurrentThread()->proc ? Core_GetCurrentThread()->proc->egid : 0;
    obos_status status = Vfs_AccessAs(euid, egid, vn, read, write, exec);
    if (obos_is_success(status))
        return OBOS_STATUS_SUCCESS;
    if (!Core_GetCurrentThread() || !Core_GetCurrentThread()->proc)
        return status;
    if (status == OBOS_STATUS_READ_ONLY)
        return OBOS_STATUS_READ_ONLY;
    for (size_t i = 0; i < Core_GetCurrentThread()->proc->groups.nEntries; i++)
    {
        status = Vfs_AccessAs(euid, Core_GetCurrentThread()->proc->groups.list[i], vn, read, write, exec);
        if (obos_is_success(status))
            return OBOS_STATUS_SUCCESS;
    }
    return status;
}

obos_status Vfs_AccessAs(uid asUid, gid asGid, vnode* vn, bool read, bool write, bool exec)
{
    if (vn->flags & VFLAGS_PTS_LOCKED)
        return OBOS_STATUS_ACCESS_DENIED;

    if (write)
    {
        drv_fs_info info = {};
        driver_header* header = Vfs_GetVnodeDriver(vn);
        mount* mount = Vfs_GetVnodeMount(vn);
        if (header && mount && header->ftable.stat_fs_info)
            header->ftable.stat_fs_info(mount->device, &info);
        if (info.flags & FS_FLAGS_RDONLY)
            return OBOS_STATUS_READ_ONLY;
    }
    
    if (vn->uid == asUid || asUid == 0)
    {
        if (!vn->perm.owner_read && read)
            return OBOS_STATUS_ACCESS_DENIED;
        else if (!vn->perm.owner_write && write)
            return OBOS_STATUS_ACCESS_DENIED; 
        else if (!vn->perm.owner_exec && exec)
            return OBOS_STATUS_ACCESS_DENIED;
        else
            return OBOS_STATUS_SUCCESS;
    }
    else if (vn->gid == asGid)
    {
        if (!vn->perm.group_read && read)
            return OBOS_STATUS_ACCESS_DENIED;
        else if (!vn->perm.group_write && write)
            return OBOS_STATUS_ACCESS_DENIED; 
        else if (!vn->perm.group_exec && exec)
            return OBOS_STATUS_ACCESS_DENIED;
        else
            return OBOS_STATUS_SUCCESS;
    }
    else
    {
        if (!vn->perm.other_read && read)
            return OBOS_STATUS_ACCESS_DENIED;
        else if (!vn->perm.other_write && write)
            return OBOS_STATUS_ACCESS_DENIED; 
        else if (!vn->perm.other_exec && exec)
            return OBOS_STATUS_ACCESS_DENIED;
        else
            return OBOS_STATUS_SUCCESS;
    }
}