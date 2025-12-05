/*
 * oboskrnl/perm.c
 *
 * Copyright (c) 2025 Omar Berrow
 *
 * Syscall permission checking utilities
 */

#include <int.h>
#include <error.h>
#include <klog.h>
#include <perm.h>
#include <memmanip.h>

#include <scheduler/schedule.h>
#include <scheduler/process.h>

#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/alloc.h>
#include <vfs/create.h>

dirent* Vfs_PermRoot;

#define CHECK_ID(id) \
do {\
    if (*id == '/' || *id == 0) return OBOS_STATUS_INVALID_ARGUMENT;\
} while(0)\

void OBOS_CapabilityInitialize()
{
    Vfs_PermRoot = VfsH_DirentLookup(OBOS_PERM_PREFIX);
    if (!Vfs_PermRoot)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "%s: Could not find directory at OBOS_PERM_PREFIX (%s) specified at build time.\n", __func__, OBOS_PERM_PREFIX);
    // we chillin
}

obos_status OBOS_CapabilityFetch(const char* id, capability* res, bool create)
{
    if (!Vfs_PermRoot)
        return OBOS_STATUS_NOT_FOUND;
    OBOS_ENSURE(res);
    OBOS_ENSURE(id);
    CHECK_ID(id);
    dirent* ent = VfsH_DirentLookupFrom(id, Vfs_PermRoot);
    if (!ent)
    {
        if (!create)
            return OBOS_STATUS_NOT_FOUND;
        capability def = {};
        def.allow_user = true;
        def.allow_group = true;
        def.owner = ROOT_UID;
        def.group = ROOT_GID;
        return OBOS_CapabilitySet(id, &def, true);
    }

    if (!ent->vnode)
        return OBOS_STATUS_INTERNAL_ERROR;

    res->owner = ent->vnode->uid;
    res->group = ent->vnode->gid;
    res->allow_user = ent->vnode->perm.owner_exec;
    res->allow_group = ent->vnode->perm.group_exec;
    res->allow_other = ent->vnode->perm.other_exec;

    if (res->allow_other)
        OBOS_Warning("Allowing 'other' permissions on capability \"%s\"\n", id);
    if (!res->allow_other && !res->allow_group && !res->allow_user)
        OBOS_Warning("Capability \"%s\" is disabled.\n", id);
    if (res->owner != ROOT_UID)
        OBOS_Warning("Capability \"%s\" has weird ownership. Owned by %d:%d\n", id, res->owner, res->group);

    return OBOS_STATUS_SUCCESS;
}

// TODO: Should this be replaced with strchr?
static size_t str_search(const char* str, char ch)
{
    size_t ret = strchr(str, ch);
    for (; str[ret] == ch && str[ret] != 0; ret++)
        ;
    return ret;
}

static obos_status create_parents(const char* path, dirent* root, dirent** last_parent, const char** last_token)
{
    size_t path_len = strlen(path);
    if (!path_len)
        return OBOS_STATUS_INVALID_ARGUMENT;

    for (; *path == '/'; path++, path_len--)
        ;
    const char* tok = path;
    size_t tok_len = strchr(tok, '/');
    if (tok_len != path_len)
        tok_len--;
    while (tok[tok_len - 1] == '/')
        tok_len--;
    if (!tok_len)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = OBOS_STATUS_SUCCESS;
    file_perm mode = {};
    mode.owner_read = true;
    mode.owner_write = true;
    mode.owner_exec = true;
    mode.group_read = true;
    mode.group_write = false;
    mode.group_exec = true;
    mode.other_read = true;
    mode.other_write = false;
    mode.other_exec = true;

    while (1)
    {
        char* tok_nul = Vfs_Malloc(tok_len+1);
        memcpy(tok_nul, tok, tok_len);

        dirent* new = VfsH_DirentLookupFrom(tok_nul, root);
        if (new)
            goto cont;

        cont:
        *last_token = tok;
        tok = tok + str_search(tok, '/');
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
        if (!tok_len)
        {
            // Last token, do not create anything
            *last_parent = root;
            Vfs_Free(tok_nul);
            break;
        }
        else
        {
            if (obos_is_error(status = Vfs_CreateNode(root, tok_nul, VNODE_TYPE_DIR, mode)))
                return status;
            root = VfsH_DirentLookupFrom(tok_nul, root);
            Vfs_Free(tok_nul);
            continue;
        }
    }

    return OBOS_STATUS_SUCCESS;
}

obos_status OBOS_CapabilitySet(const char* id, const capability* perm, bool overwrite)
{
    CHECK_ID(id);
    OBOS_ENSURE(Vfs_PermRoot);
    OBOS_ENSURE(id);
    OBOS_ENSURE(perm);
    capability tmp = {};
    obos_status status = OBOS_CapabilityFetch(id, &tmp, false);
    switch (status) {
        case OBOS_STATUS_SUCCESS: 
            if (!overwrite) return OBOS_STATUS_ALREADY_INITIALIZED;
            break;
        case OBOS_STATUS_NOT_FOUND:
            break;
        default: return status;
    }

    // We need to create the file.
    dirent* parent = nullptr;
    const char* name = nullptr;
    status = create_parents(id, Vfs_PermRoot, &parent, &name);
    if (obos_is_error(status))
        return status;
    OBOS_ENSURE(parent);
    file_perm mode = {};
    mode.owner_exec = perm->allow_user;
    mode.group_exec = perm->allow_group;
    mode.other_exec = perm->allow_other;
    return Vfs_CreateNodeOwner(parent, name, VNODE_TYPE_REG, mode, perm->owner, perm->group);
}

obos_status OBOS_CapabilityCheck(const char* id, bool def_other_allow)
{
    obos_status res = OBOS_CapabilityCheckAs(id, Core_GetCurrentThread()->proc->euid, Core_GetCurrentThread()->proc->egid, def_other_allow);
    if (obos_is_success(res))
        return OBOS_STATUS_SUCCESS;
    for (size_t i = 0; i < Core_GetCurrentThread()->proc->groups.nEntries; i++)
    {
        res = OBOS_CapabilityCheckAs(id, Core_GetCurrentThread()->proc->euid, Core_GetCurrentThread()->proc->groups.list[i], def_other_allow);
        if (obos_is_success(res))
            return OBOS_STATUS_SUCCESS;
        else if (res == OBOS_STATUS_ACCESS_DENIED)
            continue;
        else
            return res;
    }
    return OBOS_STATUS_ACCESS_DENIED;
}

obos_status OBOS_CapabilityCheckAs(const char* id, uid user, gid group, bool def_other_allow)
{
    capability res = {};
    obos_status status = OBOS_CapabilityFetch(id, &res, false);
    if (obos_is_error(status))
    {
        if (status == OBOS_STATUS_NOT_FOUND)
        {
            if (user == ROOT_UID || group == ROOT_GID) return OBOS_STATUS_SUCCESS;
            return def_other_allow ? OBOS_STATUS_SUCCESS : OBOS_STATUS_ACCESS_DENIED;
        }
        return status;
    }
    if (res.allow_user && res.owner == user)
        return OBOS_STATUS_SUCCESS;
    if (res.allow_group && res.group == group)
        return OBOS_STATUS_SUCCESS;
    return res.allow_other ? OBOS_STATUS_SUCCESS : OBOS_STATUS_ACCESS_DENIED;
}