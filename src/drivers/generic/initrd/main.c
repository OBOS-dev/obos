/*
 * drivers/generic/initrd/main.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>
#include <cmdline.h>

#include <stdarg.h>

#include <vfs/irp.h>
#include <vfs/alloc.h>
#include <vfs/vnode.h>

#include <driver_interface/header.h>
#include <driver_interface/pci.h>

#include <driver_interface/driverId.h>

#include <allocators/base.h>

#include "name.h"
#include "ustar_hdr.h"
#include "parse.h"

OBOS_PAGEABLE_FUNCTION obos_status get_blk_size(dev_desc desc, size_t* blkSize)
{
    OBOS_UNUSED(desc);
    if (!blkSize)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *blkSize = 1;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status get_max_blk_count(dev_desc desc, size_t* count);
OBOS_WEAK obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead);
OBOS_WEAK obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten);
OBOS_WEAK obos_status foreach_device(iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* u), void* u);
OBOS_WEAK obos_status query_user_readable_name(dev_desc what, const char** name); // unrequired for fs drivers.
OBOS_PAGEABLE_FUNCTION obos_status ioctl(dev_desc what, uint32_t request, void* argp)
{
    OBOS_UNUSED(what);
    OBOS_UNUSED(request);
    OBOS_UNUSED(argp);
    return OBOS_STATUS_INVALID_IOCTL;
}

void driver_cleanup_callback()
{}

OBOS_WEAK obos_status query_path(dev_desc desc, const char** path);
OBOS_WEAK obos_status path_search(dev_desc* found, void*, const char* what, dev_desc parent);
OBOS_WEAK obos_status get_linked_path(dev_desc desc, const char** found);
OBOS_WEAK obos_status move_desc_to(dev_desc desc, dev_desc new_parent, const char* name);
OBOS_WEAK obos_status mk_file(dev_desc* newDesc, dev_desc parent, void* vn, const char* name, file_type type, driver_file_perm perm);
OBOS_WEAK obos_status remove_file(dev_desc desc);
obos_status set_file_perms(dev_desc desc, driver_file_perm newperm)
{
    OBOS_UNUSED(desc);
    OBOS_UNUSED(newperm);
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status get_file_perms(dev_desc desc, driver_file_perm *perm);
obos_status set_file_owner(dev_desc desc, uid owner_uid, gid group_uid)
{
    OBOS_UNUSED(desc && owner_uid && group_uid);
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status get_file_type(dev_desc desc, file_type *type);
OBOS_WEAK obos_status list_dir(dev_desc dir, void* unused, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata);
OBOS_WEAK obos_status stat_fs_info(void *vn, drv_fs_info *info);

dev_desc irp_process_dryop(irp* req)
{
    dev_desc desc = req->desc;
    size_t blkCount = req->blkCount;
    size_t blkOffset = req->blkOffset;
    initrd_inode* ino = (void*)desc;
    if (!ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (ino->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    size_t filesize = ino->filesize;
    if (blkOffset >= filesize)
    {
        req->nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    return OBOS_STATUS_SUCCESS;
}

obos_status vnode_search(void** vn_found, dev_desc desc, void* dev_vn)
{
    OBOS_UNUSED(dev_vn);
    initrd_inode* ino = (void*)desc;
    if (!ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *vn_found = ino->vnode;
    return OBOS_STATUS_SUCCESS;
}

OBOS_WEAK obos_status submit_irp(void* /* irp* */ request_)
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
obos_status get_file_inode(dev_desc desc, uint32_t *out)
{
    initrd_inode* ino = (void*)desc;
    if (!ino)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *out = ino->ino;
    return OBOS_STATUS_SUCCESS;
}
OBOS_WEAK obos_status finalize_irp(void* /* irp* */ request_);

__attribute__((section(OBOS_DRIVER_HEADER_SECTION))) driver_header drv_hdr = {
    .magic = OBOS_DRIVER_MAGIC,
    .flags = DRIVER_HEADER_HAS_STANDARD_INTERFACES,
    .ftable = {
        .driver_cleanup_callback = driver_cleanup_callback,
        .ioctl = ioctl,
        .get_blk_size = get_blk_size,
        .get_max_blk_count = get_max_blk_count,
        .query_user_readable_name = query_user_readable_name,
        .foreach_device = foreach_device,
        .read_sync = read_sync,
        .write_sync = write_sync,
        .submit_irp = submit_irp,
        .finalize_irp = finalize_irp,

        .query_path = query_path,
        .path_search = path_search,
        .get_linked_path = get_linked_path,
        .vnode_search = vnode_search,
        .move_desc_to = move_desc_to,
        .mk_file = mk_file,
        .remove_file = remove_file,
        .get_file_perms = get_file_perms,
        .set_file_perms = set_file_perms,
        .set_file_owner = set_file_owner,
        .get_file_type = get_file_type,
        .get_file_inode = get_file_inode,
        .list_dir = list_dir,
        .stat_fs_info = stat_fs_info,
    },
    .driverName = INITRD_DRIVER_NAME,
    .version = 1,
};

initrd_inode* InitrdRoot;
size_t CurrentInodeNumber = 1;

static int64_t strrfind(const char* str, char ch)
{
    int64_t i = strlen(str);
    for (; i >= 0; i--)
        if (str[i] == ch)
           return i;
    return -1;
}

initrd_inode* create_inode_boot(const ustar_hdr* hdr)
{
    initrd_inode* ino = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(initrd_inode), nullptr);
    ino->ino = CurrentInodeNumber++;

    bool path_needs_slash = hdr->filename[0] != '/' && (*hdr->prefix && hdr->prefix[strnlen(hdr->prefix, 155)-1] != '/');
    ino->path_len = strnlen(hdr->filename, 100) + strnlen(hdr->prefix, 155) + path_needs_slash;
    ino->path_size = ino->path_len;
    ino->path = Allocate(OBOS_KernelAllocator, ino->path_len+1, nullptr);

    memcpy(ino->path, hdr->prefix, strnlen(hdr->prefix, 155));
    if (path_needs_slash)
        ino->path[strnlen(hdr->prefix, 155)] = '/';
    memcpy(ino->path + strnlen(hdr->prefix, 155) + path_needs_slash, hdr->filename, strnlen(hdr->filename, 100));
    
    ino->path[ino->path_len] = 0;
    ino->persistent = true;
    // Get our parent.
    // Also, seperate the path from the basename.
    while (ino->path[ino->path_len - 1] == '/')
        ino->path[--ino->path_len] = 0;
    int64_t index = strrfind(ino->path, '/')+1;
    ino->name_len = ino->path_len - index+1;
    ino->name_size = ino->name_len;
    ino->name = Allocate(OBOS_KernelAllocator, ino->name_len+1, nullptr);
    memcpy(ino->name, ino->path+index, ino->name_len);
    ino->name[ino->name_len] = 0;
    // ino->data = Allocate(OBOS_NonPagedPoolAllocator, ino->filesize, nullptr);
    // if (!ino->data)
    // {
    //     OBOS_Error("initrd: OOM while allocating node!\n");
    //     Free(OBOS_KernelAllocator, ino->name, ino->name_size);
    //     Free(OBOS_KernelAllocator, ino->path, ino->path_size);
    //     Free(OBOS_KernelAllocator, ino, sizeof(*ino));
    //     return nullptr;
    // }
    // memcpy(ino->data, (char*)hdr + 512, ino->filesize);
    back:
    switch (hdr->type) {
        case AREGTYPE:
        case REGTYPE:
            ino->type = FILE_TYPE_REGULAR_FILE;
            break;
        case DIRTYPE:
            ino->type = FILE_TYPE_DIRECTORY;
            break;
        case SYMTYPE:
            ino->type = FILE_TYPE_SYMBOLIC_LINK;
            break;
        case LNKTYPE:
        {
            obos_status status = OBOS_STATUS_SUCCESS;
            hdr = GetFile(hdr->linked, &status);
            if (obos_is_error(status))
                return nullptr;
            goto back;
            break;
        }
        default:
            OBOS_ENSURE(!"Unrecognized header type");
    }
    ino->filesize = oct2bin(hdr->filesize, strnlen(hdr->filesize, 12));
    ino->data = (char*)hdr + 512;
    
    uint16_t filemode = oct2bin(hdr->filemode, strnlen(hdr->filemode, 8));
    ino->perm.group_read = filemode & FILEMODE_GROUP_READ;
    ino->perm.owner_read = filemode & FILEMODE_OWNER_READ;
    ino->perm.other_read = filemode & FILEMODE_OTHER_READ;
    ino->perm.group_write = filemode & FILEMODE_GROUP_WRITE;
    ino->perm.owner_write = filemode & FILEMODE_OWNER_WRITE;
    ino->perm.other_write = filemode & FILEMODE_OTHER_WRITE;
    ino->perm.group_exec = filemode & FILEMODE_GROUP_EXEC;
    ino->perm.owner_exec = filemode & FILEMODE_OWNER_EXEC;
    ino->perm.other_exec = filemode & FILEMODE_OTHER_EXEC;
    ino->perm.set_uid = filemode & 04000;
    ino->perm.set_gid = filemode & 02000;
    ino->linked_path = hdr->linked;

    ino->vnode = Vfs_Calloc(1, sizeof(vnode));
    ino->vnode->desc = (uintptr_t)ino;
    ino->vnode->filesize = ino->filesize;
    ino->vnode->blkSize = 1;
    ino->vnode->owner_uid = 0;
    ino->vnode->group_uid = 0;
    ino->vnode->inode = ino->ino;
    ino->vnode->perm = ino->perm;
    ino->vnode->times.change = oct2bin(hdr->last_mod, strnlen(hdr->last_mod, 12));
    ino->vnode->times.birth = ino->vnode->times.change;
    ino->vnode->times.access = ino->vnode->times.change;
    switch (ino->type) {
        case FILE_TYPE_REGULAR_FILE:
            ino->vnode->vtype = VNODE_TYPE_REG;
            break;
        case FILE_TYPE_DIRECTORY:
            ino->vnode->vtype = VNODE_TYPE_DIR;
            break;
        case FILE_TYPE_SYMBOLIC_LINK:
            ino->vnode->vtype = VNODE_TYPE_LNK;
            ino->vnode->un.linked = ino->linked_path;
            break;
        default:
            OBOS_UNREACHABLE;
    }

    ino->hdr = hdr;

    return ino;
}
driver_init_status OBOS_DriverEntry(driver_id* this)
{
    (void)this;

    // for (volatile bool b = true; b; )
    //     ;

    // Parse the initrd, and make the initrd_inode tree.

    InitrdRoot = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(initrd_inode), nullptr);
    InitrdRoot->filesize = 0;
    InitrdRoot->name = "";
    InitrdRoot->path = "";
    InitrdRoot->type = FILE_TYPE_DIRECTORY;
    InitrdRoot->persistent = false;
    
    InitrdRoot->perm.owner_exec = true;
    InitrdRoot->perm.group_exec = true;
    InitrdRoot->perm.other_exec = false;

    InitrdRoot->perm.owner_read = true;
    InitrdRoot->perm.group_read = true;
    InitrdRoot->perm.other_read = false;
    
    InitrdRoot->perm.owner_write = true;
    InitrdRoot->perm.group_write = true;
    InitrdRoot->perm.other_write = false;

    InitrdRoot->ino = CurrentInodeNumber++;

    const ustar_hdr* hdr = (ustar_hdr*)OBOS_InitrdBinary;
    if (OBOS_InitrdSize < sizeof(ustar_hdr))
        return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false};

    while (memcmp(hdr->magic, USTAR_MAGIC, 6))
    {
        size_t hdr_filename_len = strnlen(hdr->filename, 100);
        size_t hdr_prefix_len = strnlen(hdr->prefix, 155);
        size_t filesize = oct2bin(hdr->filesize, strnlen(hdr->filesize, 12));
        if (strchr(hdr->filename, '/') != hdr_filename_len || strchr(hdr->prefix, '/') != hdr_prefix_len)
            goto down;

        initrd_inode* ino = create_inode_boot(hdr);
        ino->parent = InitrdRoot;
        if (!ino->parent->children.head)
            ino->parent->children.head = ino;
        if (ino->parent->children.tail)
            ino->parent->children.tail->next = ino;
        ino->prev = ino->parent->children.tail;
        ino->parent->children.tail = ino;
        ino->parent->children.nChildren++;

        down:
        (void)0;
        size_t filesize_rounded = (filesize + 0x1ff) & ~0x1ff;
        hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
    }

    return (driver_init_status){.status=OBOS_STATUS_SUCCESS,.fatal=false};
}

// dev_desc is simply a pointer to a ustar_hdr

OBOS_PAGEABLE_FUNCTION obos_status get_max_blk_count(dev_desc desc, size_t* count)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !count)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (desc == UINTPTR_MAX)
        return OBOS_STATUS_NOT_A_FILE;
    if (inode->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    *count = inode->filesize;
    return OBOS_STATUS_SUCCESS;
}
OBOS_PAGEABLE_FUNCTION obos_status read_sync(dev_desc desc, void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkRead)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (inode->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    if (blkOffset >= inode->filesize)
    {
        if (nBlkRead) *nBlkRead = 0;
        return OBOS_STATUS_SUCCESS;
    }
    size_t nToRead = blkCount;
    if ((blkOffset + blkCount) >= inode->filesize)
        nToRead = inode->filesize - blkOffset;
    const char* iter = inode->data+blkOffset;
    memcpy(buf, iter, nToRead);
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status query_path(dev_desc desc, const char** path)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !path)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *path = inode->path;
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status get_linked_path(dev_desc desc, const char** found)
{
    initrd_inode* ino = (void*)desc;
    if (ino->type != FILE_TYPE_SYMBOLIC_LINK)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *found = ino->linked_path;
    return OBOS_STATUS_SUCCESS;
}

obos_status move_desc_to(dev_desc desc, dev_desc dnew_parent, const char* name)
{
    initrd_inode* ino = (void*)desc;
    initrd_inode* new_parent = (void*)dnew_parent;
    if (dnew_parent == UINTPTR_MAX)
        new_parent = InitrdRoot;
    
    if (!ino) return OBOS_STATUS_INVALID_ARGUMENT;

    // FIXME: This does not handle persistent inodes.
    // This is problematic because if the old name is accessed,
    // then it will be recreated.
    if (new_parent)
    {
        if (ino->next)
            ino->next->prev = ino->prev;
        if (ino->prev)
            ino->prev->next = ino->next;
        if (ino->parent->children.head)
            ino->parent->children.head = ino->next;
        if (ino->parent->children.tail)
            ino->parent->children.tail = ino->prev;
        ino->parent->children.nChildren--;
        ino->parent = new_parent;
        if (!ino->parent->children.head)
            ino->parent->children.head = ino;
        if (ino->parent->children.tail)
            ino->parent->children.tail->next = ino;
        ino->prev = ino->parent->children.tail;
        ino->parent->children.tail = ino;
        ino->parent->children.nChildren++;
    }
    if (name)
    {
        ino->name_len = 0;
        Free(OBOS_KernelAllocator, ino->name, ino->name_size);
        ino->name_size = ino->name_len = strlen(name);
        ino->name_size++;
        ino->name = memcpy(Allocate(OBOS_KernelAllocator, ino->name_size, nullptr), name, ino->name_len);
        
        ino->path_len = 0;
        Free(OBOS_KernelAllocator, ino->name, ino->path_size);
        ino->path_len = snprintf(nullptr, 0, "%.*s%c%s", 
            ino->parent->path_len, ino->parent->path,
            (ino->parent->path[ino->parent->path_len-1] == '/' ? '\0' : '/'),
            ino->name);
        ino->path_size = ino->path_len + 1;
        ino->path = Allocate(OBOS_KernelAllocator, ino->path_size, nullptr);
        snprintf(ino->path, ino->path_size, "%.*s%c%s", 
            ino->parent->path_len, ino->parent->path,
            (ino->parent->path[ino->parent->path_len-1] == '/' ? '\0' : '/'),
            ino->name);
    }

    return OBOS_STATUS_SUCCESS;
}

static char* fullpath(dev_desc parent, const char* what)
{
    char *ret = nullptr;
    if (parent != UINTPTR_MAX)
    {
        size_t ppath_len = strlen(((initrd_inode*)parent)->path);
        const char* format = (((initrd_inode*)parent)->path)[ppath_len-1] == '/' ?
                                "%s%s" : "%s/%s";
        size_t len = snprintf(nullptr, 0, format, ((initrd_inode*)parent)->path, what);
        ret = Allocate(OBOS_KernelAllocator, len+1, nullptr);
        snprintf(ret, len+1, format, ((initrd_inode*)parent)->path, what);
    }
    else
    {
        size_t len = strlen(what);
        ret = memcpy(Allocate(OBOS_KernelAllocator, len+1, nullptr), what, len+1);
    }
    OBOS_ENSURE(ret);
    return ret;
}
initrd_inode* create_inode_with_parents(const char* path, const ustar_hdr* hdr)
{
    if (!hdr)
        hdr = GetFile(path, nullptr);
    if (!hdr)
        return nullptr;
    
    initrd_inode* ino = create_inode_boot(hdr);
    // Get our parent.
    int64_t index = strrfind(ino->path, '/');
    if (index == -1)
        index = 0;
    char presv = ino->path[index];
    ino->path[index] = 0;
    ino->parent = DirentLookupFrom(ino->path /* really the 'dirname' */, InitrdRoot);
    ino->path[index] = presv;

    if (!ino->parent)
    {
        ino->parent = InitrdRoot;
        // Create all parent directories
        char* iter = ino->path;
        char* end = ino->path + ino->path_len;
        while (iter < end)
        {
            size_t off = strchr(iter, '/');
            presv = iter[off];
            iter[off] = 0;
            initrd_inode* found = DirentLookupFrom(ino->path, InitrdRoot);
            if (found)
            {
                if (found->dead)
                    found->dead = false;
                ino->parent = found;
                goto down;
            }

            const ustar_hdr *sub_hdr = GetFile(ino->path, nullptr);
            if (!sub_hdr && ((iter + off) >= end))
                goto down; // NOTE: buggy?
            if (sub_hdr == hdr)
                goto down;
            
            initrd_inode* sub_ino = create_inode_boot(sub_hdr);
            sub_ino->parent = ino->parent;
            // printf("%s %s\n", iter, sub_ino->parent->name);
            if (!sub_ino->parent->children.head)
                sub_ino->parent->children.head = sub_ino;
            if (sub_ino->parent->children.tail)
                sub_ino->parent->children.tail->next = sub_ino;
            sub_ino->prev = sub_ino->parent->children.tail;
            sub_ino->parent->children.tail = sub_ino;
            sub_ino->parent->children.nChildren++;
            
            ino->parent = sub_ino;

            down:
            iter[off] = presv;
            iter += off;
        }
    }
    if (!ino->parent)
        ino->parent = InitrdRoot;
    if (!ino->parent->children.head)
        ino->parent->children.head = ino;
    if (ino->parent->children.tail)
        ino->parent->children.tail->next = ino;
    ino->prev = ino->parent->children.tail;
    ino->parent->children.tail = ino;
    ino->parent->children.nChildren++;

    return ino;
}
OBOS_PAGEABLE_FUNCTION obos_status path_search(dev_desc* found, void* unused, const char* what, dev_desc parent)
{
    OBOS_UNUSED(unused);
    if (!found || !what)
        return OBOS_STATUS_INVALID_ARGUMENT;
    initrd_inode* ino = DirentLookupFrom(what, parent == UINTPTR_MAX ? InitrdRoot : (initrd_inode*)parent);
    if (ino)
    {
        if (ino->dead)
        {
            *found = 0;
            return OBOS_STATUS_NOT_FOUND;
        }
        *found = (dev_desc)ino;
        return OBOS_STATUS_SUCCESS;
    }
    char* path = fullpath(parent, what);
    ino = create_inode_with_parents(path, nullptr);
    Free(OBOS_KernelAllocator, path, strlen(path));

    *found = (dev_desc)ino;
    return ino ? OBOS_STATUS_SUCCESS : OBOS_STATUS_NOT_FOUND;
}

OBOS_PAGEABLE_FUNCTION obos_status get_file_perms(dev_desc desc, driver_file_perm *perm)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !perm)
        return OBOS_STATUS_INVALID_ARGUMENT;
    *perm = inode->perm;
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status get_file_type(dev_desc desc, file_type *type)
{
    if (!type || !desc)
        return OBOS_STATUS_INVALID_ARGUMENT;
    initrd_inode* inode = (void*)desc;
    *type = inode->type;
    return OBOS_STATUS_SUCCESS;
}

OBOS_PAGEABLE_FUNCTION obos_status list_dir(dev_desc dir_, void* unused, iterate_decision(*cb)(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name), void* userdata)
{
    OBOS_UNUSED(unused);
    if (!dir_)
        return OBOS_STATUS_INVALID_ARGUMENT;
    initrd_inode* dir = dir_ == UINTPTR_MAX ? InitrdRoot : (void*)dir_;
    const ustar_hdr* hdr = (ustar_hdr*)OBOS_InitrdBinary;
    // FIXME: Non persistent entries won't appear here.
    if (dir->persistent)
    {
        while (memcmp(hdr->magic, USTAR_MAGIC, 6))
        {
            size_t filesize = oct2bin(hdr->filesize, strnlen(hdr->filesize, 12));
            if (hdr == dir->hdr)
                goto down;
            char* hdr_path = nullptr;
            size_t hdr_path_len = strnlen(hdr->filename, 100) + strnlen(hdr->prefix, 155);
            do {
                size_t len_hdr_prefix = strnlen(hdr->prefix, 155);
                bool prefix_has_slash = true;
                if (*hdr->prefix && hdr->prefix[len_hdr_prefix-1] != '/' && hdr->filename[0] != '/')
                {
                    hdr_path_len++;
                    prefix_has_slash = false;
                }
                hdr_path = Allocate(OBOS_KernelAllocator, hdr_path_len+1, nullptr);
                snprintf(hdr_path, hdr_path_len+1, prefix_has_slash ? "%s%s" : "%s/%s", hdr->prefix, hdr->filename);
            } while(0);

            if (!dir->path_len || (strncmp(dir->path, hdr_path, dir->path_len) && dir->path_len != hdr_path_len))
            {
                int addend = 0;
                if (dir->path_len)
                    addend = dir->path[dir->path_len-1] != '/';
                if (strchr(hdr_path+dir->path_len+addend, '/') == hdr_path_len-(dir->path_len+addend))
                {
                    initrd_inode *ino = DirentLookupFrom(hdr_path, dir);
                    if (!ino)
                        ino = create_inode_with_parents(hdr_path, hdr);
                    if (ino->dead)
                        goto down;
                    if (cb((dev_desc)ino, 1, ino->filesize, userdata, ino->name) == ITERATE_DECISION_STOP)
                    {
                        Free(OBOS_KernelAllocator, hdr_path, hdr_path_len+1);
                        break;
                    }
                }
            }
            Free(OBOS_KernelAllocator, hdr_path, hdr_path_len+1);

            down:
            (void)0;
            size_t filesize_rounded = (filesize + 0x1ff) & ~0x1ff;
            hdr = (ustar_hdr*)(((uintptr_t)hdr) + filesize_rounded + 512);
        }
    }
    else
    {
        for (initrd_inode* ino = dir->children.head; ino; )
        {
            if (ino->dead)
                goto next;
            if (cb((dev_desc)ino, 1, ino->filesize, userdata, ino->name) == ITERATE_DECISION_STOP)
                break;
            next:
            ino = ino->next;
        }
    }
    return OBOS_STATUS_SUCCESS;
}

// static iterate_decision cb(dev_desc desc, size_t blkSize, size_t blkCount, void* userdata, const char* name)
// {
//     OBOS_UNUSED(blkSize);
//     OBOS_UNUSED(blkCount);
//     OBOS_UNUSED(name);

//     size_t* const fileCount = userdata;
//     (*fileCount)++;

//     initrd_inode* ino = (void*)desc;
//     if (ino->type == FILE_TYPE_DIRECTORY)
//         list_dir(desc, nullptr, cb, userdata);
//     return ITERATE_DECISION_CONTINUE;
// }

obos_status stat_fs_info(void *vn, drv_fs_info *info)
{
    OBOS_UNUSED(vn);
    static size_t fileCount = 0;
    // if (fileCount == SIZE_MAX)
    // {
    //     fileCount = 0;
    //     list_dir(UINTPTR_MAX, vn, cb, &fileCount);
    // }
    info->partBlockSize = 1;
    info->fsBlockSize = 1;
    info->availableFiles = 0;
    info->freeBlocks = 0;
    info->fileCount = fileCount;
    info->szFs = OBOS_InitrdSize;
    info->flags = 0;
    // TODO: Is there a proper value for this?
    info->nameMax = 100;
    return OBOS_STATUS_SUCCESS;
}

obos_status mk_file(dev_desc* newDesc, dev_desc parent_desc, void* vn, const char* name, file_type type, driver_file_perm perm)
{
    OBOS_UNUSED(vn);
    initrd_inode *parent = (initrd_inode*)parent_desc;
    if (parent_desc == UINTPTR_MAX)
        parent = InitrdRoot;
    if (!parent_desc || !newDesc || !name)
        return OBOS_STATUS_INVALID_ARGUMENT;

    initrd_inode *new = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(initrd_inode), nullptr);
    new->name_size = new->name_len = strlen(name);
    new->type = type;
    new->perm = perm;
    new->ino = CurrentInodeNumber++;
    new->name = Allocate(OBOS_KernelAllocator, new->name_len+1, nullptr);
    memcpy(new->name, name, new->name_len);
    new->name[new->name_len] = 0;
    new->path_len = parent->path_len + 1 + new->name_len;
    new->path_size = new->path_len;
    new->path = Allocate(OBOS_KernelAllocator, new->path_len+1, nullptr);
    memcpy(new->path, parent->path, parent->path_len);
    new->path[parent->path_len] = '/';
    memcpy(&new->path[parent->path_len+1], new->name, new->name_len);
    new->path[new->path_len] = 0;
    // printf("created new node: parent.path: %s, new.path: %s, new.name: %s\n", parent->path, new->path, new->name);

    new->parent = parent;
    if (!parent->children.head)
        parent->children.head = new;
    if (parent->children.tail)
        parent->children.tail->next = new;
    new->prev = parent->children.tail;
    parent->children.tail = new;
    parent->children.nChildren++;

    *newDesc = (dev_desc)new;
    return OBOS_STATUS_SUCCESS;
}

obos_status write_sync(dev_desc desc, const void* buf, size_t blkCount, size_t blkOffset, size_t* nBlkWritten)
{
    initrd_inode* inode = (void*)desc;
    if (!inode || !buf)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!blkCount)
        return OBOS_STATUS_SUCCESS;
    if (inode->type != FILE_TYPE_REGULAR_FILE)
        return OBOS_STATUS_NOT_A_FILE;
    size_t nToExpand = ((blkOffset + blkCount) > inode->filesize) ? (blkOffset + blkCount) - inode->filesize : 0;
    inode->filesize += nToExpand;
    if (inode->persistent)
    {
        char* new_data = Allocate(OBOS_NonPagedPoolAllocator, inode->filesize, nullptr);
        memcpy(new_data, inode->data, inode->filesize);
        inode->data = new_data;
        inode->persistent = false;
    }
    else
        inode->data = Reallocate(OBOS_NonPagedPoolAllocator, inode->data, inode->filesize, inode->filesize-nToExpand, nullptr);
    memcpy(inode->data+blkOffset, buf, blkCount);
    if (nBlkWritten)
        *nBlkWritten = blkCount;
    return OBOS_STATUS_SUCCESS;
}

obos_status remove_file(dev_desc desc)
{
    initrd_inode* inode = (void*)desc;
    if (!inode)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (inode->parent)
    {
        if (!inode->persistent)
        {
            if (inode->next)
                inode->next->prev = inode->prev;
            if (inode->prev)
                inode->prev->next = inode->next;
            if (!inode->prev)
                inode->parent->children.head = inode->next;
            if (!inode->next)
                inode->parent->children.tail = inode->prev;
            inode->parent->children.nChildren--;
        }
        else
            inode->dead = true;
    }
    if (!inode->persistent)
    {
        Free(OBOS_NonPagedPoolAllocator, inode->data, inode->filesize);
        Free(OBOS_KernelAllocator, inode, sizeof(*inode));
    }
    return OBOS_STATUS_SUCCESS;
}
