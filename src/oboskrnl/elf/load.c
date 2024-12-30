/*
 * oboskrnl/elf/load.c
 *
 * Copyright (c) 2024 Omar Berrow
 */

#include <int.h>
#include <error.h>
#include <memmanip.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <elf/elf.h>
#include <elf/load.h>

#include <vfs/fd.h>
#include <vfs/vnode.h>
#include <vfs/mount.h>

// Do it in two passes so that any macros can be expanded
#define token_concat_impl(tok1, tok2) tok1 ##tok2
#define token_concat(tok1, tok2) token_concat_impl(tok1, tok2)
#define GetCurrentElfClass() token_concat(ELFCLASS, OBOS_ARCHITECTURE_BITS)

static obos_status verify_elf(const void* file, size_t szFile)
{
    const Elf_Ehdr* ehdr = file;
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3)
        return OBOS_STATUS_INVALID_FILE;

    if (ehdr->e_ident[EI_CLASS] != GetCurrentElfClass())
        return OBOS_STATUS_INVALID_FILE;

    uint8_t ei_data_val = 0;
    if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Little-Endian"))
        ei_data_val = ELFDATA2LSB;
    else if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Big-Endian"))
        ei_data_val = ELFDATA2MSB;
    else
        ei_data_val = ELFDATANONE;
    if (ehdr->e_ident[EI_DATA] != ei_data_val)
        return OBOS_STATUS_INVALID_FILE;

    if (ehdr->e_machine != EM_CURRENT)
        return OBOS_STATUS_INVALID_FILE;

    if ((ehdr->e_phoff > szFile) || ((ehdr->e_phoff+ehdr->e_phentsize*ehdr->e_phnum) > szFile))
        return OBOS_STATUS_INVALID_FILE;

    return OBOS_STATUS_SUCCESS;
}

obos_status OBOS_LoadELF(context* ctx, const void* file, size_t szFile, elf_info* info, bool dryRun, bool noLdr)
{
    if (!ctx || !file || !szFile)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (!info && !dryRun)
        return OBOS_STATUS_INVALID_ARGUMENT;

    obos_status status = verify_elf(file, szFile);
    if (obos_is_error(status))
        return status;

    const Elf_Ehdr* ehdr = file;

    bool load_dynld = false;

    switch (ehdr->e_type) {
        case ET_DYN:
        case ET_EXEC:
            load_dynld = !noLdr;
            break;
        default:
            return OBOS_STATUS_INVALID_FILE;
    }

    load:
    (void)0; // thanks clang
    uintptr_t limit = 0;
    uintptr_t base = 0;
    bool first_load = true;
    const Elf_Phdr* phdrs = (Elf_Phdr*)((const char*)file+ehdr->e_phoff);
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        if (phdrs[i].p_offset > szFile)
            return OBOS_STATUS_INVALID_FILE;
        if ((phdrs[i].p_offset + phdrs[i].p_filesz) > szFile)
            return OBOS_STATUS_INVALID_FILE;
        if ((phdrs[i].p_vaddr+ phdrs[i].p_memsz) > limit)
            limit = phdrs[i].p_vaddr + phdrs[i].p_memsz;
        if (phdrs[i].p_vaddr <= base || first_load)
            base = phdrs[i].p_vaddr;
        first_load = false;
    }

    if (dryRun)
        goto dry;

    uintptr_t real_entry = 0;
    if (load_dynld)
    {
        const Elf_Phdr* phdrs = (Elf_Phdr*)((const char*)file+ehdr->e_phoff);
        const char* interp = nullptr;
        for (size_t i = 0; i < ehdr->e_phnum; i++)
        {
            if (phdrs[i].p_type == PT_INTERP)
            {
                interp = (char*)file+phdrs[i].p_offset;
                break;
            }
        }

        if (!interp)
        {
            load_dynld = false;
            goto load;
        }

        fd interp_fd = {};
        status = Vfs_FdOpen(&interp_fd, interp, FD_OFLAGS_READ);
        if (obos_is_error(status))
            return status;

        // Load the interpreter instead, but also make sure to sanity check it.
        // Assume the interpreter doesn't need an interpreter, that'd be pretty dumb.

        if (!VfsH_LockMountpoint(interp_fd.vn->mount_point))
            return OBOS_STATUS_INTERNAL_ERROR;
        volatile size_t buff_size = interp_fd.vn->filesize;
        VfsH_UnlockMountpoint(interp_fd.vn->mount_point);

        status = OBOS_STATUS_SUCCESS;
        void* buff = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, buff_size, OBOS_PROTECTION_READ_ONLY, VMA_FLAGS_PRIVATE, &interp_fd, &status);
        if (obos_is_error(status))
        {
            Vfs_FdClose(&interp_fd);
            return status;
        }
        Vfs_FdClose(&interp_fd);

        elf_info tmp_info = {};
        status = OBOS_LoadELF(ctx, buff, buff_size, &tmp_info, dryRun, true);
        if (obos_is_error(status))
            return status;
        real_entry = tmp_info.entry;
        Mm_VirtualMemoryFree(&Mm_KernelContext, (void*)buff, buff_size);
    }

    limit += (OBOS_PAGE_SIZE-(limit%OBOS_PAGE_SIZE));
    base -= (base%OBOS_PAGE_SIZE);

    bool require_addend = false;
    if (!base)
    {
        obos_status status = OBOS_STATUS_SUCCESS;
        base = (uintptr_t)Mm_VirtualMemoryAlloc(ctx, nullptr, limit-base, 0, 0, nullptr, &status);
        if (!base)
            return status;
        require_addend = true;
        limit += base;
    }

    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        void* kbase = nullptr;
        void* ubase = nullptr;
        obos_status status = OBOS_STATUS_SUCCESS;
        if (!require_addend)
        {
            uintptr_t real_base = phdrs[i].p_vaddr;
            real_base -= (real_base%OBOS_PAGE_SIZE);
            uintptr_t real_limit = phdrs[i].p_vaddr+phdrs[i].p_memsz;
            if (real_limit % OBOS_PAGE_SIZE)
                real_limit += (OBOS_PAGE_SIZE-(real_limit%OBOS_PAGE_SIZE));
            ubase = Mm_VirtualMemoryAlloc(ctx,
                                          (void*)real_base,
                                          real_limit-real_base,
                                          0, 0, nullptr,
                                          &status) + (phdrs[i].p_vaddr%OBOS_PAGE_SIZE);
        }
        else
            ubase = (void*)(phdrs[i].p_vaddr + base);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }

        uintptr_t real_base = phdrs[i].p_vaddr;
        real_base -= (real_base%OBOS_PAGE_SIZE);
        uintptr_t real_limit = phdrs[i].p_vaddr+phdrs[i].p_memsz;
        if (real_limit % OBOS_PAGE_SIZE)
            real_limit += (OBOS_PAGE_SIZE-(real_limit%OBOS_PAGE_SIZE));
        if (require_addend)
        {
            real_base += base;
            real_limit += base;
        }
        __builtin_printf("%p %p %p %d\n", (void*)real_base, (void*)real_limit, (void*)base, require_addend);
        kbase = Mm_MapViewOfUserMemory(ctx,
                                       (void*)real_base,
                                       nullptr,
                                       real_limit-real_base,
                                       0,
                                       false, // disrespect user protection flags.
                                       &status) + (phdrs[i].p_vaddr % OBOS_PAGE_SIZE);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }

        memcpy(kbase, (void*)((uintptr_t)file + phdrs[i].p_offset), phdrs[i].p_filesz);
        if (phdrs[i].p_memsz-phdrs[i].p_filesz)
            memzero((void*)((uintptr_t)kbase+phdrs[i].p_filesz), phdrs[i].p_memsz-phdrs[i].p_filesz);
        Mm_VirtualMemoryFree(&Mm_KernelContext, kbase, phdrs[i].p_memsz);

        prot_flags prot = OBOS_PROTECTION_USER_PAGE;
        if (phdrs[i].p_flags & PF_X)
            prot |= OBOS_PROTECTION_EXECUTABLE;
        if (~phdrs[i].p_flags & PF_W)
            prot |= OBOS_PROTECTION_READ_ONLY;

        status = Mm_VirtualMemoryProtect(ctx, ubase, phdrs[i].p_memsz, prot, true);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }
    }

    info->base = (void*)base;
    info->entry = (require_addend ? base : 0) + ehdr->e_entry;
    info->real_entry = real_entry ? real_entry : info->entry;

    dry:

    return OBOS_STATUS_SUCCESS;
}
