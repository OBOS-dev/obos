/*
 * oboskrnl/vfs/tmpfs.c
 *
 * Copyright (c) 2026 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <klog.h>
#include <perm.h>

#include <vfs/alloc.h>
#include <vfs/dirent.h>
#include <vfs/irp.h>
#include <vfs/mount.h>
#include <vfs/tmpfs.h>
#include <vfs/vnode.h>
#include <vfs/create.h>

#include <driver_interface/header.h>

#define TMPFS_GET_FILE_FS(vn) ((tmpfs*)(vn->mount_point->device->data))

static obos_status get_blk_size(dev_desc desc, size_t* blkSize) { OBOS_UNUSED(desc); *blkSize = 1; return OBOS_STATUS_SUCCESS; }
static obos_status get_max_blk_count(dev_desc desc, size_t* count) { if (desc != UINTPTR_MAX) *count = ((vnode*)desc)->filesize; else return OBOS_STATUS_NOT_A_FILE; return OBOS_STATUS_SUCCESS; }

static obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    vnode* vn = (void*)desc;
    tmpfs* fs = TMPFS_GET_FILE_FS(vn);
    if (!fs->backend || !vn->tmpfs_secondary_desc)
    {
        memzero(buf, blkCount);
        if (nBlkRead) *nBlkRead = blkCount;
        return OBOS_STATUS_SUCCESS;
    }
    return fs->backend->ftable.read_sync(vn->tmpfs_secondary_desc, buf, blkCount, blkOffset, nBlkRead);
}
static obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    OBOS_UNUSED(desc && buf && blkCount && blkOffset && nBlkWritten);
    return OBOS_STATUS_SUCCESS;
}

static obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}

static void driver_cleanup_callback()
{}

static obos_status symlink_set_path(dev_desc desc, const char* to) { OBOS_UNUSED(desc && to); return OBOS_STATUS_SUCCESS; }

static obos_status path_searchd(dirent** found, void* vn, const char* what, dev_desc parent, bool return_placeholders)
{
    if (!what || !vn || !found)
        return OBOS_STATUS_INVALID_ARGUMENT;

    while (*what == '/')
        what++;

    tmpfs* fs = ((vnode*)vn)->data;

    if (parent == UINTPTR_MAX)
        parent = (dev_desc)fs->root->vnode;

    dirent* p = ((vnode*)parent)->data;
    
    dirent* ent = VfsH_DirentLookupFromCacheOnly(what, p);
    if (ent)
    {
        if (ent->flags & DIRENT_TMPFS_FILE_MOVED && !return_placeholders)
            return OBOS_STATUS_NOT_FOUND;
        if (~ent->flags & DIRENT_TMPFS_FILE_MOVED && ent->vnode->vtype == VNODE_TYPE_DIR)
            ent->vnode->data = ent;
        *found = ent;
        return OBOS_STATUS_SUCCESS;
    }

    if (!fs->backend)
        return OBOS_STATUS_NOT_FOUND;

    dev_desc sdesc = -1;
    if (((vnode*)parent)->tmpfs_secondary_desc)
    {
        obos_status status = fs->backend->ftable.path_search(&sdesc, fs->backend_fs, what, ((vnode*)parent)->tmpfs_secondary_desc);
        if (obos_is_error(status))
            return status;
    }
    else
        return OBOS_STATUS_NOT_FOUND;

    char* path = nullptr;
    size_t path_len = strlen(what);
    path = memcpy(Vfs_Malloc(path_len+1), what, path_len+1);

    dirent* last = ((vnode*)parent)->data;
    char* iter = path;
    while (iter < (path+path_len))
    {
        size_t tok_len = strchr(iter, '/');
        if (iter[tok_len] != '\0') tok_len--;
        if (!tok_len)
            goto cont;

        for (dirent* child = last->d_children.head; child; )
        {
            if (OBOS_CompareStringNC(&child->name, iter, tok_len))
            {
                last = child;
                goto cont;
            }

            child = child->d_next_child;
        }

        dev_desc current_desc = 0;
        const char ch = iter[tok_len];
        iter[tok_len] = 0;
        
        obos_status status = last->vnode->tmpfs_secondary_desc ? fs->backend->ftable.path_search(&current_desc, fs->backend_fs, iter, last->vnode->tmpfs_secondary_desc) : OBOS_STATUS_SUCCESS;
        OBOS_ENSURE(obos_is_success(status));
        
        iter[tok_len] = ch;

        dirent* ent = Vfs_Calloc(1, sizeof(dirent));
        OBOS_InitStringLen(&ent->name, iter, tok_len);

        vnode* new = Vfs_Calloc(1, sizeof(vnode));
        new->blkSize = 1;
        
        file_type type = 0;
        fs->backend->ftable.get_file_perms(current_desc, &new->perm);
        fs->backend->ftable.get_file_type(current_desc, &type);
        switch (type)
        {
            case FILE_TYPE_REGULAR_FILE:
                new->vtype = VNODE_TYPE_REG;
                fs->backend->ftable.get_max_blk_count(current_desc, &new->filesize);
                break;
            case FILE_TYPE_DIRECTORY:
                new->vtype = VNODE_TYPE_DIR;
                break;
            case FILE_TYPE_SYMBOLIC_LINK:
                new->vtype = VNODE_TYPE_LNK;
                break;
            default:
                OBOS_ASSERT(type);
        }
        new->mount_point = fs->root->vnode->mount_point;
        new->desc = (dev_desc)new;
        new->inode = fs->next_unused_inode++;
        new->tmpfs_secondary_desc = current_desc;
        new->mount_point = fs->root->vnode->mount_point;
        if (current_desc)
            fs->backend->ftable.get_linked_path(current_desc, &new->un.linked);

        if (new->vtype == VNODE_TYPE_DIR)
            new->data = ent;

        ent->vnode = new;

        if (ent->vnode->vtype == VNODE_TYPE_DIR)
            ent->vnode->data = ent;

        VfsH_DirentAppendChild(last, ent);
        last = ent;

        cont:
        iter = iter + tok_len + 1;
    }

    *found = last;

    return last ? OBOS_STATUS_SUCCESS : OBOS_STATUS_NOT_FOUND;
}

static obos_status path_search(dev_desc* found, void* vn, const char* what, dev_desc parent)
{
    dirent* last = nullptr;

    obos_status status = path_searchd(&last, vn, what, parent, false);
    if (obos_is_error(status))
        return status;

    *found = (dev_desc)last->vnode;
    
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_linked_path(dev_desc desc, const char** found)
{
    vnode* vn = (void*)desc;
    tmpfs* fs = TMPFS_GET_FILE_FS(vn);

    if (vn->vtype != VNODE_TYPE_LNK)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (!fs->backend || vn->un.linked)
        *found = vn->un.linked;
    else if (fs->backend->ftable.get_linked_path)
        return vn->tmpfs_secondary_desc ? fs->backend->ftable.get_linked_path(vn->tmpfs_secondary_desc, found) : OBOS_STATUS_INVALID_ARGUMENT;
    else
        return OBOS_STATUS_INTERNAL_ERROR;

    return OBOS_STATUS_SUCCESS;
}

static obos_status move_desc_to(void* vn, const char* path, const char* newpath, const char* name)
{
    dirent* ent = nullptr;
    dirent* parent = nullptr;
    dirent* old_parent = nullptr;
    dirent* replacement_dirent = nullptr;
    dirent* placeholder = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;

    if (!path)
        return OBOS_STATUS_INVALID_ARGUMENT;

    status = path_searchd(&ent, vn, path, UINTPTR_MAX, false);
    if (obos_is_error(status))
        return status;
    old_parent = ent->d_parent;

    status = newpath ? path_searchd(&parent, vn, newpath, UINTPTR_MAX, false) : OBOS_STATUS_SUCCESS;
    if (obos_is_error(status))
        return status;

    // If newpath/name exists, and is a placeholder (i.e., DIRENT_TMPFS_FILE_MOVED is set), remove the placeholder.
    placeholder = VfsH_DirentLookupFromCacheOnly(name, parent ? parent : ent->d_parent);
    if (placeholder)
    {
        if (~placeholder->flags & DIRENT_TMPFS_FILE_MOVED)
            return OBOS_STATUS_ALREADY_INITIALIZED;
        
        VfsH_DirentRemoveChild(placeholder->d_parent, placeholder);
        OBOS_FreeString(&placeholder->name);
        Vfs_Free(placeholder);
    }
    
    replacement_dirent = Vfs_Calloc(1, sizeof(dirent));
    replacement_dirent->flags = DIRENT_TMPFS_FILE_MOVED;
    replacement_dirent->vnode = nullptr;
    OBOS_InitStringLen(&replacement_dirent->name, OBOS_GetStringCPtr(&ent->name), OBOS_GetStringSize(&ent->name));
    
    status = Vfs_RenameNode(ent, parent, name, true);
    if (obos_is_error(status))
    {
        OBOS_FreeString(&replacement_dirent->name);
        Vfs_Free(replacement_dirent);
        return status;
    }
    
    if (parent == old_parent && OBOS_CompareStringC(&replacement_dirent->name, name))
    {
        OBOS_FreeString(&replacement_dirent->name);
        Vfs_Free(replacement_dirent);
    }
    else
        VfsH_DirentAppendChild(old_parent, replacement_dirent);

    return OBOS_STATUS_SUCCESS;
}

static obos_status mk_file(dev_desc* newDesc, const char* parent_path, void* vn, const char* name, file_type type, driver_file_perm perm)
{
    tmpfs* fs = ((vnode*)vn)->data;
    dirent* real_parent = nullptr;
    dirent* ent = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;
    uint32_t vtype = (type == FILE_TYPE_REGULAR_FILE) ? VNODE_TYPE_REG : ((type == FILE_TYPE_DIRECTORY) ? VNODE_TYPE_DIR : VNODE_TYPE_LNK);

    if (obos_is_error(status = path_searchd(&real_parent, vn, parent_path, UINTPTR_MAX, false)))
        return status;

    if (obos_is_success(path_searchd(&ent, vn, name, (dev_desc)real_parent->vnode, true)))
    {
        if (~ent->flags & DIRENT_TMPFS_FILE_MOVED)
            return OBOS_STATUS_ALREADY_INITIALIZED;

        VfsH_DirentRemoveChild(ent->d_parent, ent);
        OBOS_FreeString(&ent->name);
        Vfs_Free(ent);
    }

    if (real_parent->vnode->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;

    status = Vfs_CreateNode(real_parent, name, vtype | BIT(31), perm);
    if (obos_is_error(status))
        return status;

    ent = VfsH_DirentLookupFromCacheOnly(name, real_parent);
    OBOS_ENSURE(ent);
    *newDesc = (dev_desc)ent->vnode;
    ent->vnode->mount_point = fs->root->vnode->mount_point;
    ent->vnode->desc = (dev_desc)ent->vnode;
    if (vtype == VNODE_TYPE_DIR)
        ent->vnode->data = ent;

    return OBOS_STATUS_SUCCESS;
}

static obos_status remove_file(void* vn, const char* path)
{
    dirent* ent = nullptr;
    dirent* replacement_dirent = nullptr;
    dirent* old_parent = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;

    status = path_searchd(&ent, vn, path, UINTPTR_MAX, false);
    if (obos_is_error(status))
        return status;
    old_parent = ent->d_parent;

    replacement_dirent = Vfs_Calloc(1, sizeof(dirent));
    replacement_dirent->flags = DIRENT_TMPFS_FILE_MOVED;
    replacement_dirent->vnode = nullptr;
    OBOS_InitStringLen(&replacement_dirent->name, OBOS_GetStringCPtr(&ent->name), OBOS_GetStringSize(&ent->name));

    ent->vnode->flags |= VFLAGS_TMPFS_FILE_DEAD;
    status = Vfs_UnlinkNode(ent, true);

    if (obos_is_error(status))
    {
        OBOS_FreeString(&replacement_dirent->name);
        Vfs_Free(replacement_dirent);
    }
    else
        VfsH_DirentAppendChild(old_parent, replacement_dirent);

    return status;
}

static obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(newperm);
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_file_perms(dev_desc desc, driver_file_perm *perm)
{
    vnode* vn = (vnode*)desc;
    *perm = vn->perm;
    return OBOS_STATUS_SUCCESS;
}

static obos_status set_file_owner(dev_desc desc, uid owner_uid, gid group_uid)
{
    OBOS_UNUSED(desc && owner_uid && group_uid);
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_file_type(dev_desc desc, file_type *type)
{
    vnode* vn = (vnode*)desc;
    switch (vn->vtype) {
        case VNODE_TYPE_REG: *type = FILE_TYPE_REGULAR_FILE; break;
        case VNODE_TYPE_DIR: *type = FILE_TYPE_DIRECTORY; break;
        case VNODE_TYPE_LNK: *type = FILE_TYPE_SYMBOLIC_LINK; break;
        default: return OBOS_STATUS_INVALID_ARGUMENT;
    }
    return OBOS_STATUS_SUCCESS;
}

static iterate_decision populate_cb(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name)
{   
    OBOS_UNUSED(blkSize && blkCount);
    uintptr_t* fuserdata = userdata;
    dirent* const dent = (void*)fuserdata[0];
    for (dirent* child = dent->d_children.head; child; )
    {
        if (OBOS_CompareStringC(&child->name, name))
            return ITERATE_DECISION_CONTINUE;

        child = child->d_next_child;
    }

    tmpfs* fs = (tmpfs*)fuserdata[1];
    mount* point = (mount*)fuserdata[2];
    
    vnode* vn = Vfs_Calloc(1, sizeof(*vn));

    vn->filesize = blkCount;
    vn->blkSize = 1;
    vn->inode = fs->next_unused_inode++;
    if (fs->backend->ftable.get_file_owner)
        fs->backend->ftable.get_file_owner(desc, &vn->uid, &vn->gid);
    
    file_type type = -1;
    if (fs->backend->ftable.get_file_perms)
        fs->backend->ftable.get_file_perms(desc, &vn->perm);
    if (fs->backend->ftable.get_file_type)
        fs->backend->ftable.get_file_type(desc, &type);
    switch (type)
    {
        case FILE_TYPE_REGULAR_FILE:
            vn->vtype = VNODE_TYPE_REG;
            break;
        case FILE_TYPE_DIRECTORY:
            vn->vtype = VNODE_TYPE_DIR;
            break;
        case FILE_TYPE_SYMBOLIC_LINK:
            vn->vtype = VNODE_TYPE_LNK;
            fs->backend->ftable.get_linked_path(desc, &vn->un.linked);
            break;
        default:
            OBOS_ENSURE(!"invalid type");
    }
    vn->mount_point = point;
    vn->desc = (dev_desc)vn;
    vn->inode = fs->next_unused_inode++;
    
    dirent* new = Vfs_Calloc(1, sizeof(dirent));
    OBOS_StringSetAllocator(&new->name, Vfs_Allocator);
    OBOS_InitString(&new->name, name);
    new->vnode = vn;
    if (vn->vtype == VNODE_TYPE_DIR)
        vn->data = new;
    VfsH_DirentAppendChild(dent, new);
    vn->tmpfs_secondary_desc = desc;
    // LIST_APPEND(dirent_list, &point->dirent_list, new);
    return ITERATE_DECISION_CONTINUE;
}

obos_status list_dir(dev_desc dir, void* dev_vn, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata)
{
    OBOS_UNUSED(dev_vn);

    tmpfs* fs = ((vnode*)dev_vn)->data;
    vnode* vn = (vnode*)(dir == UINTPTR_MAX ? (dev_desc)fs->root->vnode : dir);

    if (vn->vtype != VNODE_TYPE_DIR)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (vn->flags & VFLAGS_TMPFS_FILE_DEAD)
        return OBOS_STATUS_NOT_FOUND;

    OBOS_ENSURE(vn->data);
    uintptr_t fuserdata[3] = {
        (uintptr_t)vn->data,
        (uintptr_t)fs,
        (uintptr_t)vn->mount_point
    };
    if (fs->backend && vn->tmpfs_secondary_desc)
        fs->backend->ftable.list_dir(vn->tmpfs_secondary_desc, fs->backend_fs, populate_cb, fuserdata);

    for (dirent* ent = ((dirent*)(vn->data))->d_children.head; ent; ent = ent->d_next_child)
    {
        if (ent->flags & DIRENT_TMPFS_FILE_MOVED)
            continue;
        if (cb((dev_desc)ent->vnode, 1, ent->vnode->filesize, userdata, OBOS_GetStringCPtr(&ent->name)) == ITERATE_DECISION_STOP)
            break;
    }
    
    return OBOS_STATUS_SUCCESS;
}

static obos_status stat_fs_info(OBOS_MAYBE_UNUSED void *vn, drv_fs_info *info)
{
    info->availableFiles = SIZE_MAX;
    info->fsBlockSize = 1;
    info->partBlockSize = 1;
    info->freeBlocks = SIZE_MAX;
    info->fileCount = 0;
    info->nameMax = 255;
    info->szFs = SIZE_MAX;
    return OBOS_STATUS_SUCCESS;
}

static dev_desc irp_process_dryop(irp* req)
{
    dev_desc desc = req->desc;
    size_t blkCount = req->blkCount;
    size_t blkOffset = req->blkOffset;
    vnode* vno = (void*)desc;
    if (!vno)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (vno->vtype != VNODE_TYPE_REG)
        return OBOS_STATUS_NOT_A_FILE;
    size_t filesize = vno->filesize;
    if (blkOffset >= filesize)
    {
        req->nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    return OBOS_STATUS_SUCCESS;
}

static obos_status vnode_search(void** vn_found, dev_desc desc, void* dev_vn)
{
    OBOS_UNUSED(dev_vn);
    OBOS_ASSERT(desc != UINTPTR_MAX);
    *vn_found = (void*)desc;
    return OBOS_STATUS_SUCCESS;
}

static obos_status submit_irp(void* /* irp* */ request_)
{
    if (!request_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    irp* request = request_;
    if (request->dryOp)
        request->status = irp_process_dryop(request);
    else
        request->status = request->op == IRP_READ ? 
            read_sync(request->desc, request->buff, request->blkCount, request->blkOffset, &request->nBlkRead) :
            write_sync(request->desc, request->cbuff, request->blkCount, request->blkOffset, &request->nBlkRead);
    request->evnt = nullptr;
    return OBOS_STATUS_SUCCESS;
}

static obos_status get_file_inode(dev_desc desc, uint32_t *out)
{
    vnode* vn = (void*)desc;
    if (vn->inode)
        *out = vn->inode;
    else
        *out = ++TMPFS_GET_FILE_FS(vn)->next_unused_inode;
    return OBOS_STATUS_SUCCESS;
}

static obos_status trunc_file(dev_desc desc, size_t newsize /* note, newsize must be less than the filesize */)
{
    OBOS_UNUSED(desc && newsize);
    return OBOS_STATUS_SUCCESS;
}

static obos_status mount_p(void* vnp, void* targetp)
{
    vnode* vn = vnp;
    dirent* target = targetp;

    tmpfs* fs = vn->data;

    fs->root->vnode->mount_point = target->vnode->mount_point;
    
    return OBOS_STATUS_SUCCESS;
}

static obos_status hardlink_file(dev_desc desc, const char* parent_path, void* vn, const char* name)
{
    dirent* real_parent = nullptr;
    dirent* ent = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;

    if (obos_is_error(status = path_searchd(&real_parent, vn, parent_path, UINTPTR_MAX, false)))
        return status;

    if (obos_is_success(path_searchd(&ent, vn, name, (dev_desc)real_parent->vnode, true)))
    {
        if (~ent->flags & DIRENT_TMPFS_FILE_MOVED)
            return OBOS_STATUS_ALREADY_INITIALIZED;

        VfsH_DirentRemoveChild(ent->d_parent, ent);
        OBOS_FreeString(&ent->name);
        Vfs_Free(ent);
    }

    ent = Vfs_Calloc(1, sizeof(dirent));
    ent->vnode = (vnode*)desc;
    OBOS_InitString(&ent->name, name);
    VfsH_DirentAppendChild(real_parent, ent);

    return OBOS_STATUS_SUCCESS;
}

driver_id OBOS_TmpFSDriver = {
    .id=0,
    .header = {
        .magic = OBOS_DRIVER_MAGIC,
        .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES|DRIVER_HEADER_DIRENT_CB_PATHS,
        .ftable = {
            .driver_cleanup_callback = driver_cleanup_callback,
            .ioctl = ioctl,
            .get_blk_size = get_blk_size,
            .get_max_blk_count = get_max_blk_count,
            .query_user_readable_name = nullptr,
            .foreach_device = nullptr,
            .read_sync = read_sync,
            .write_sync = write_sync,
            .submit_irp = submit_irp,
            .finalize_irp = nullptr,

            .query_path = nullptr,
            .path_search = path_search,
            .get_linked_path = get_linked_path,
            .vnode_search = vnode_search,
            .pmove_desc_to = move_desc_to,
            .pmk_file = mk_file,
            .premove_file = remove_file,
            .get_file_perms = get_file_perms,
            .set_file_perms = set_file_perms,
            .set_file_owner = set_file_owner,
            .get_file_type = get_file_type,
            .get_file_inode = get_file_inode,
            .list_dir = list_dir,
            .stat_fs_info = stat_fs_info,
            .symlink_set_path = symlink_set_path,
            .trunc_file = trunc_file,
            .mount = mount_p,
            .phardlink_file = hardlink_file,
        },
        .driverName = "TmpFS Driver",
        .version = 1,
    }
};

obos_status Vfs_TmpFSCreate(vnode** tmpfso, driver_header* backend, void* backend_fs)
{
    vnode* tmpfsv = Vfs_Calloc(1, sizeof(*tmpfsv));
    tmpfs* fs = nullptr;
    obos_status status = OBOS_STATUS_SUCCESS;
    capability cap = {};

    status = Vfs_TmpFSInitializeSpecial(&fs, backend, backend_fs);
    if (obos_is_error(status))
        return status;

    tmpfsv->data = fs;
    tmpfsv->blkSize = 1;
    tmpfsv->filesize = -1;
    tmpfsv->flags |= VFLAGS_TMPFS;
    tmpfsv->vtype = VNODE_TYPE_BLK;
    status = OBOS_CapabilityFetch("fs/tmpfs-perm", &cap, false);
    if (status == OBOS_STATUS_NOT_FOUND)
    {
        cap.allow_user = true;
        cap.allow_group = false;
        cap.allow_other = false;
        cap.owner = ROOT_UID;
        cap.group = ROOT_GID;
    }
    else if (obos_is_error(status))
    {
        OBOS_Error("%s: OBOS_CapabilityFetch returned %d\n", __func__, status);
        return OBOS_STATUS_INTERNAL_ERROR;
    }
    tmpfsv->perm.owner_read = cap.allow_user;
    tmpfsv->perm.owner_write = cap.allow_user;
    tmpfsv->perm.group_read = cap.allow_group;
    tmpfsv->perm.group_write = cap.allow_group;
    tmpfsv->perm.other_read = cap.allow_other;
    tmpfsv->perm.other_write = cap.allow_other;
    tmpfsv->uid = cap.owner;
    tmpfsv->gid = cap.group;
    fs->obj = tmpfsv;

    *tmpfso = tmpfsv;
    
    return OBOS_STATUS_SUCCESS;
}

obos_status Vfs_TmpFSInitializeSpecial(tmpfs** out, driver_header* backend, void* backend_fs)
{
    if (!out)
        return OBOS_STATUS_INVALID_ARGUMENT;
    
    *out = Vfs_Calloc(1, sizeof(tmpfs));
    tmpfs* fs = *out;
    fs->next_unused_inode = 2;
    fs->backend = backend;
    fs->backend_fs = backend_fs;
    
    fs->root = Vfs_Calloc(1, sizeof(*fs->root));
    OBOS_InitStringLen(&fs->root->name, "/", 1);
    fs->root->vnode = Vfs_Calloc(1, sizeof(vnode));
    fs->root->vnode->blkSize = 1;
    fs->root->vnode->filesize = 0;
    fs->root->vnode->vtype = VNODE_TYPE_DIR;
    fs->root->vnode->desc = (dev_desc)fs->root->vnode;
    fs->root->vnode->tmpfs_secondary_desc = UINTPTR_MAX;
    fs->root->vnode->data = fs->root;
    fs->root->vnode->gid = 0;
    fs->root->vnode->uid = 0;
    fs->root->vnode->inode = fs->next_unused_inode++;
    fs->root->vnode->mount_point = nullptr;
    fs->root->vnode->perm = (file_perm){.mode=0777};

    return OBOS_STATUS_SUCCESS;
}

obos_status Vfs_TmpFSMakeVnode(tmpfs* fs, vnode* vn, dirent* ent)
{
    if (!fs || !vn || !ent)
        return OBOS_STATUS_INVALID_ARGUMENT;

    if (vn->vtype == VNODE_TYPE_DIR)
        vn->data = ent;
    vn->inode = fs->next_unused_inode++;

    return OBOS_STATUS_SUCCESS;
}
