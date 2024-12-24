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


// Do it in two passes so that any macros can be expanded
#define token_concat_impl(tok1, tok2) tok1 ##tok2
#define token_concat(tok1, tok2) token_concat_impl(tok1, tok2)
#define GetCurrentElfClass() token_concat(ELFCLASS, OBOS_ARCHITECTURE_BITS)

obos_status OBOS_LoadELF(context* ctx, const void* file, size_t szFile)
{
    if (!ctx || !file || !szFile)
        return OBOS_STATUS_INVALID_ARGUMENT;
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

    uintptr_t limit = 0;
    uintptr_t base = 0;
    Elf_Phdr* phdrs = (Elf_Phdr*)((char*)file+ehdr->e_phoff);
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdrs[i].p_type != PT_LOAD)
            continue;
        if (phdrs[i].p_vaddr > limit)
            limit = phdrs[i].p_vaddr;
        if (phdrs[i].p_vaddr <= base)
            base = phdrs[i].p_vaddr + phdrs[i].p_memsz;
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
            ubase = Mm_VirtualMemoryAlloc(ctx, (void*)phdrs[i].p_vaddr, phdrs[i].p_memsz, 0, 0, nullptr, &status);
        else
            ubase = (void*)(phdrs[i].p_vaddr + base);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }
        status = Mm_MapViewOfUserMemory(ctx,
                                        ubase,
                                        (kbase = MmH_FindAvailableAddress(&Mm_KernelContext, phdrs[i].p_memsz, 0, nullptr)),
                                        phdrs[i].p_memsz,
                                        0,
                                        false);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }
        memcpy(kbase, ubase, phdrs[i].p_filesz);
        if (phdrs[i].p_memsz-phdrs[i].p_filesz)
            memzero((void*)((uintptr_t)kbase+phdrs[i].p_filesz), phdrs[i].p_memsz-phdrs[i].p_filesz);
        Mm_VirtualMemoryFree(ctx, kbase, phdrs[i].p_memsz);
        prot_flags prot = OBOS_PROTECTION_USER_PAGE;
        if (phdrs[i].p_flags & PF_X)
            prot |= OBOS_PROTECTION_EXECUTABLE;
        if (~phdrs[i].p_flags & PF_W)
            prot |= OBOS_PROTECTION_READ_ONLY;
        status = Mm_VirtualMemoryProtect(ctx, kbase, phdrs[i].p_memsz, prot, true);
        if (obos_is_error(status))
        {
            // Clean up.
            for (size_t j = 0; j < i; j++)
                Mm_VirtualMemoryFree(ctx, (void*)phdrs[j].p_vaddr, phdrs[j].p_memsz);
            return status;
        }
    }
    return OBOS_STATUS_SUCCESS;
}
