/*
 * oboskrnl/arch/m68k/driver_loader.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <driver_interface/driverId.h>

#include <mm/context.h>
#include <mm/alloc.h>

#include <elf/elf.h>

#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>

#define OffsetPtr(ptr, off, type) (type)(((uintptr_t)ptr) + ((intptr_t)off))
// Please forgive me for this
#define Cast(what, to) ((to)what)

// Helpers.
struct relocation_table
{
    Elf_Dyn* table;
    size_t nEntries;
    bool rel; // false if rela, true if rel.
};
struct relocation
{
    uint32_t symbolTableOffset;
    uintptr_t virtualAddress;
    uint16_t relocationType;
    int64_t addend;
};
struct relocation_array
{
    struct relocation_table* buf;
    size_t nRelocations;
};
struct copy_reloc
{
	void* src, *dest;
	size_t size;
};
struct copy_reloc_array
{
    struct copy_reloc* buf;
    size_t nRelocations;
};
static void append_relocation_table(struct relocation_array* arr, const struct relocation_table* what)
{
    arr->buf = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, arr->buf, (++arr->nRelocations)*sizeof(*arr->buf), nullptr);
    OBOS_ASSERT(arr->buf);
    arr->buf[arr->nRelocations - 1] = *what; 
}
static void free_reloc_array(struct relocation_array* arr)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, arr->buf, arr->nRelocations*sizeof(*arr->buf));
    memzero(arr, sizeof(*arr));
}
static void append_copy_reloc(struct copy_reloc_array* arr, const struct copy_reloc* what)
{
    arr->buf = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, arr->buf, (++arr->nRelocations)*sizeof(*arr->buf), nullptr);
    OBOS_ASSERT(arr->buf);
    arr->buf[arr->nRelocations - 1] = *what; 
}
static void free_copy_reloc_array(struct copy_reloc_array* arr)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, arr->buf, arr->nRelocations*sizeof(*arr->buf));
    memzero(arr, sizeof(*arr));
}

// From https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-48031.html#scrolltoc
static uint32_t ElfHash(const char* name)
{
    uint32_t h = 0, g = 0;

    while (*name)
    {
        h = (h << 4) + *name++;
        if ((g = h & 0xf0000000))
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}
static Elf32_Sym* GetSymbolFromTable(
			const uint8_t* fileStart,
			uint8_t* baseAddress,
			Elf32_Sym* symbolTable,
			uintptr_t hashTableOff,
			Elf32_Off stringTable,
			const char* _symbol)
{
    Elf32_Word* hashTableBase = (Elf32_Word*)(baseAddress + hashTableOff);
    Elf32_Word nBuckets = hashTableBase[0];
    uint32_t currentBucket = ElfHash(_symbol) % nBuckets;
    Elf32_Word* firstBucket = hashTableBase + 2;
    Elf32_Word* firstChain = firstBucket + nBuckets;
    size_t index = firstBucket[currentBucket];
    while (index)
    {
        Elf32_Sym* symbol = &symbolTable[index];
        const char* symbolName = (char*)(fileStart + stringTable + symbol->st_name);
        if (strcmp(symbolName, _symbol))
            return symbol;
        
        index = firstChain[index];
    }
    return nullptr;
}

static void add_dependency(driver_id* depends, driver_id* dependency)
{
    if (!dependency || !depends)
        return;
    // depends is the driver that depends on the dependency.
    for (driver_node* cur = depends->dependencies.head; cur; )
    {
        if (cur->data == dependency)
            return; // don't add an already added dependency to the list
        cur = cur->next;
    }
    driver_node* node = OBOS_KernelAllocator->Allocate(OBOS_KernelAllocator, sizeof(driver_node), nullptr);
    node->data = dependency;
    driver_list* list = &depends->dependencies;
    if (!list->head)
        list->head = node;
    if (list->tail)
        list->tail->next = node;
    node->prev = list->tail;
    list->tail = node;
    list->nNodes++;
    dependency->refCnt++;
}
static bool calculate_relocation(obos_status* status, driver_id* drv, Elf_Sym* symbolTable, Elf32_Off stringTable, const void* file, struct relocation i, void* base, size_t szProgram, Elf32_Addr* GOT, struct copy_reloc_array* copy_relocations, uintptr_t hashTableOffset)
{
    if (status)
        *status = OBOS_STATUS_SUCCESS;
    driver_symbol* Symbol;
    driver_symbol internal_symbol = {};
    if (i.symbolTableOffset)
    {
        Elf32_Sym* Unresolved_Symbol = &symbolTable[i.symbolTableOffset];
        driver_id* dependency = nullptr;
        Symbol = DrvH_ResolveSymbol(
            OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*),
            &dependency
        );
        if (Symbol && Symbol->visibility != SYMBOL_VISIBILITY_DEFAULT)
        {
            if (status)
                *status = OBOS_STATUS_DRIVER_REFERENCED_UNRESOLVED_SYMBOL;
            OBOS_Debug("Could not resolve symbol '%s' (symbol is hidden) referenced within a driver.\n", OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*));
            Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
            return false;
        }
        if (!Symbol)
        {
            Elf32_Sym* sym = GetSymbolFromTable(
                file, base, symbolTable, hashTableOffset, stringTable,
                OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*)
            );
            if (sym->st_shndx == 0)
                goto unresolved;
            internal_symbol.address = OffsetPtr(base, sym->st_value, uintptr_t);
            internal_symbol.size = sym->st_size;
            internal_symbol.visibility = SYMBOL_VISIBILITY_DEFAULT;
            switch (ELF32_ST_TYPE(sym->st_info)) 
            {
                case STT_FUNC:
                    internal_symbol.type = SYMBOL_TYPE_FUNCTION;
                    break;
                case STT_FILE:
                    internal_symbol.type = SYMBOL_TYPE_FILE;
                    break;
                case STT_OBJECT:
                    internal_symbol.type = SYMBOL_TYPE_VARIABLE;
                    break;
                default:
                    break;
            }
            Symbol = &internal_symbol;
        }
        unresolved:
        if (!Symbol && ELF32_ST_BIND(Unresolved_Symbol->st_info) != STB_WEAK) 
        {
            if (status)
                *status = OBOS_STATUS_DRIVER_REFERENCED_UNRESOLVED_SYMBOL;
            OBOS_Debug("Could not resolve symbol '%s' referenced within a driver.\n", OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*));
            Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
            return false;
        }
        add_dependency(drv, dependency);
        if (ELF32_ST_BIND(Unresolved_Symbol->st_info) == STB_WEAK)
        {
            internal_symbol.size = Unresolved_Symbol->st_size;
            internal_symbol.address = 0;
            switch (ELF32_ST_TYPE(Unresolved_Symbol->st_info)) 
            {
                case STT_FUNC:
                    internal_symbol.type = SYMBOL_TYPE_FUNCTION;
                    break;
                case STT_FILE:
                    internal_symbol.type = SYMBOL_TYPE_FILE;
                    break;
                case STT_OBJECT:
                    internal_symbol.type = SYMBOL_TYPE_VARIABLE;
                    break;
                default:
                    break;
            }
            internal_symbol.visibility = SYMBOL_VISIBILITY_DEFAULT;
            Symbol = &internal_symbol;
        }
        if (Unresolved_Symbol->st_size != Symbol->size && i.relocationType == R_68K_COPY)
        {
            // Oh no!
            if (status)
                *status = OBOS_STATUS_DRIVER_SYMBOL_MISMATCH;
            Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
            return false;
        }
    }
    uintptr_t relocAddr = (uintptr_t)base + i.virtualAddress;
    uint32_t relocResult = 0;
    uint8_t relocSize = 0;
    switch (i.relocationType)
    {
        case R_68K_NONE:
            break;
        case R_68K_32:
            relocResult = Symbol->address + i.addend;
            relocSize = 4;
            break;
        case R_68K_16:
            relocResult = Symbol->address + i.addend;
            relocSize = 2;
            break;
        case R_68K_8:
            relocResult = Symbol->address + i.addend;
            relocSize = 1;
            break;
        case R_68K_PC32:
            relocResult = Symbol->address + i.addend - relocAddr;
            relocSize = 1;
            break;
        case R_68K_PC16:
            relocResult = Symbol->address + i.addend - relocAddr;
            relocSize = 2;
            break;
        case R_68K_PC8:
            relocResult = Symbol->address + i.addend - relocAddr;
            relocSize = 1;
            break;
        case R_68K_GOT32:
            relocResult = (uintptr_t)GOT + i.addend - relocAddr;
            relocSize = 4;
            break;
        case R_68K_GOT16:
            relocResult = (uintptr_t)GOT + i.addend - relocAddr;
            relocSize = 2;
            break;
        case R_68K_GOT8:
            relocResult = (uintptr_t)GOT + i.addend - relocAddr;
            relocSize = 1;
            break;
        case R_68K_GOT320:
            relocResult = 0; // (uintptr_t)GOT - (uintptr_t)GOT
            relocSize = 4;
            break;
        case R_68K_GOT160:
            relocResult = 0; // (uintptr_t)GOT - (uintptr_t)GOT
            relocSize = 2;
            break;
        case R_68K_GOT80:
            relocResult = 0; // (uintptr_t)GOT - (uintptr_t)GOT
            relocSize = 1;
            break;
        case R_68K_PLT32:
            // TODO: Get PLT address.
            if (status)
                *status = OBOS_STATUS_UNIMPLEMENTED;
            return false;
            relocResult = 0;
            relocSize = 4;
            break;
        case R_68K_PLT16:
            // TODO: Get PLT address.
            if (status)
                *status = OBOS_STATUS_UNIMPLEMENTED;
            return false;
            relocResult = 0;
            relocSize = 2;
            break;
        case R_68K_PLT8:
            // TODO: Get PLT address.
            if (status)
                *status = OBOS_STATUS_UNIMPLEMENTED;
            return false;
            relocResult = 0;
            relocSize = 1;
            break;
        case R_68K_PLT320:
            relocResult = 0; // PLT-PLT
            relocSize = 4;
            break;
        case R_68K_PLT160:
            relocResult = 0; // PLT-PLT
            relocSize = 2;
            break;
        case R_68K_PLT80:
            relocResult = 0; // PLT-PLT
            relocSize = 2;
            break;
        case R_68K_COPY:
        {
            // Save copy relocations for the end because if we don't, it might contain unresolved addresses.
            struct copy_reloc reloc = { (void*)relocAddr, (void*)Symbol->address, Symbol->size };
            append_copy_reloc(copy_relocations, &reloc);
            relocSize = 0;
            break;
        }
        case R_68K_GLOB_DAT:
        case R_68K_JUMP_SLOT:
            relocResult = Symbol->address;
            relocSize = 4;
            break;
        case R_68K_RELATIVE:
            relocResult = (uint32_t)base + i.addend;
            relocSize = 4;
            break;
        default:
            break;
    }
    switch (relocSize)
    {
    case 0:
        break; // The relocation type is rather unsupported or special.
    case 1: // word8
        *(uint8_t*)(relocAddr) = (uint8_t)(relocResult & 0xff);
        break;
    case 2: // word16
        *(uint16_t*)(relocAddr) = (uint16_t)(relocResult & 0xffff);
        break;
    case 4: // word32
        *(uint32_t*)(relocAddr) = (uint32_t)(relocResult & 0xffffffff);
        break;
    default:
        break;
    }
    // memcpy((void*)relocAddr, &relocResult, relocSize);
    return true;
}
void dyn_loader_dummy() { return; }
void* DrvS_LoadRelocatableElf(driver_id* driver, const void* file, size_t szFile, Elf_Sym** dynamicSymbolTable, size_t* nEntriesDynamicSymbolTable, const char** dynstrtab, void** top, obos_status* status)
{
    OBOS_UNUSED(driver);
    OBOS_UNUSED(file);
    OBOS_UNUSED(szFile);
    OBOS_UNUSED(dynamicSymbolTable);
    OBOS_UNUSED(nEntriesDynamicSymbolTable);
    OBOS_UNUSED(dynstrtab);
    OBOS_UNUSED(top);
    OBOS_UNUSED(status);
    Elf_Ehdr* ehdr = Cast(file, Elf_Ehdr*);
    Elf_Phdr* phdr_table = OffsetPtr(ehdr, ehdr->e_phoff, Elf_Phdr*);
    Elf_Phdr* dynamic = nullptr;
    size_t szProgram = 0;
    char* end = nullptr;
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr_table[i].p_type == PT_DYNAMIC)
            dynamic = &phdr_table[i];
        if (phdr_table[i].p_type != PT_LOAD)
            continue;
        Elf_Phdr* curr = &phdr_table[i];
        if (!curr->p_memsz)
            continue;
        if (!end || curr->p_vaddr > (uintptr_t)end)
			end = (void*)(curr->p_vaddr + ((curr->p_memsz + 0xfff) & ~0xfff));
    }
    end = (void*)(((uintptr_t)end + 0xfff) & ~0xfff);
    szProgram = (size_t)end;
    void* base = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, szProgram, 0, VMA_FLAGS_GUARD_PAGE, status);
    if (!base)
        return nullptr;
    // The pages are mapped in.
    // Copy the phdr data into them.
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr_table[i].p_type != PT_LOAD &&
            // phdr_table[i].p_type != PT_PHDR &&
            phdr_table[i].p_type != PT_DYNAMIC
        )
            continue;
        Elf_Phdr* curr = &phdr_table[i];
        if (!curr->p_memsz)
            continue;
        memcpy(OffsetPtr(base, curr->p_vaddr, void*), OffsetPtr(file, curr->p_offset, void*), curr->p_filesz);
        // NOTE: Possible buffer overflow
        memzero(OffsetPtr(base, curr->p_vaddr + curr->p_filesz, void*), curr->p_memsz - curr->p_filesz); 
    }
    // Find the relocations in the dynamic header.
    struct relocation_array relocations = {};
    Elf32_Sym* symbolTable = 0;
	Elf32_Off stringTable = 0;
    Elf32_Addr hashTableOffset = 0;
    Elf32_Addr* GOT = nullptr;
    size_t last_dtrelasz = 0, last_dtrelsz = 0, last_dtpltrelsz = 0;
    bool awaitingRelaSz = false, foundRelaSz = false;
    Elf_Dyn* dynEntryAwaitingRelaSz = nullptr;
    bool awaitingRelSz = false, foundRelSz = false;
    Elf_Dyn* dynEntryAwaitingRelSz = nullptr;
    uint64_t last_dlpltrel = 0;
    void* dyn_base = OffsetPtr(base, dynamic->p_vaddr, Elf32_Dyn*);
    struct relocation_table current = {};
    struct relocation_table *last_found_dtrel = {};
    struct relocation_table *last_found_dtrela = {};
    struct relocation_table *last_found_dtjmprel = {};
    OBOS_UNUSED(last_found_dtrel);
    OBOS_UNUSED(last_found_dtrela);
    OBOS_UNUSED(last_found_dtjmprel);
    for (Elf_Dyn* currentDynamicHeader = dyn_base; currentDynamicHeader->d_tag != DT_NULL; currentDynamicHeader++)
    {
        switch (currentDynamicHeader->d_tag)
        {
        case DT_HASH:
            hashTableOffset = currentDynamicHeader->d_un.d_ptr;
            break;
        case DT_PLTGOT:
            // TODO: Find out whether this is the PLT or GOT (if possible).
            GOT = (Elf32_Addr*)(base + currentDynamicHeader->d_un.d_ptr);
            break;
        case DT_REL:
            if (!foundRelSz)
            {
                awaitingRelSz = true;
                dynEntryAwaitingRelSz = currentDynamicHeader;
                break;
            }
            current.rel = true;
            current.table = currentDynamicHeader;
            current.nEntries = last_dtrelsz/sizeof(Elf32_Rel);
            append_relocation_table(&relocations, &current);
            foundRelSz = false;
            last_dtrelsz = 0;
            last_found_dtrel = &relocations.buf[relocations.nRelocations - 1];
            break;
        case DT_RELA:
            if (!foundRelaSz)
            {
                awaitingRelaSz = true;
                dynEntryAwaitingRelaSz = currentDynamicHeader;
                break;
            }
            current.rel = false;
            current.table = currentDynamicHeader;
            current.nEntries = last_dtrelasz/sizeof(Elf32_Rela);
            append_relocation_table(&relocations, &current);
            foundRelaSz = false;
            last_dtrelasz = 0;
            last_found_dtrela = &relocations.buf[relocations.nRelocations - 1];
            break;
        case DT_JMPREL:
            switch (last_dlpltrel)
            {
            case DT_REL:
                current.rel = true;
                current.table = currentDynamicHeader;
                current.nEntries = last_dtpltrelsz/sizeof(Elf32_Rel);
                append_relocation_table(&relocations, &current);
                break;
            case DT_RELA:
                current.rel = false;
                current.table = currentDynamicHeader;
                current.nEntries = last_dtpltrelsz/sizeof(Elf32_Rela);
                append_relocation_table(&relocations, &current);
                break;
            default:
                break;
            }
            last_found_dtjmprel = &relocations.buf[relocations.nRelocations - 1];
            break;
        case DT_RELSZ:
            last_dtrelsz = currentDynamicHeader->d_un.d_val;
            foundRelSz = !awaitingRelSz;
            if (awaitingRelSz)
            {
                current.rel = true;
                current.table = dynEntryAwaitingRelSz;
                current.nEntries = last_dtrelsz/sizeof(Elf32_Rel);
                append_relocation_table(&relocations, &current);
                last_found_dtrel = &relocations.buf[relocations.nRelocations - 1];
                awaitingRelSz = false;
            }
            break;
        case DT_RELASZ:
            last_dtrelasz = currentDynamicHeader->d_un.d_val;
            foundRelaSz = !awaitingRelaSz;
            if (awaitingRelaSz)
            {
                current.rel = false;
                current.table = dynEntryAwaitingRelaSz;
                current.nEntries = last_dtrelasz/sizeof(Elf32_Rela);
                append_relocation_table(&relocations, &current);
                last_found_dtrela = &relocations.buf[relocations.nRelocations - 1];
                awaitingRelaSz = false;
            }
            break;
        case DT_PLTREL:
            last_dlpltrel = currentDynamicHeader->d_un.d_val;
            break;
        case DT_PLTRELSZ:
            last_dtpltrelsz = currentDynamicHeader->d_un.d_val;
            break;
        case DT_STRTAB:
            stringTable = currentDynamicHeader->d_un.d_ptr;
            break;
        case DT_SYMTAB:
            symbolTable = (Elf32_Sym*)(base + currentDynamicHeader->d_un.d_ptr);
            break;
        // case DT_RELACOUNT:
        //     last_found_dtrela->nEntries = currentDynamicHeader->d_un.d_val;
        //     break;
        // case DT_RELCOUNT:
        //     last_found_dtrel->nEntries = currentDynamicHeader->d_un.d_val;
        //     break;
        default:
            break;
        }
    }
    struct copy_reloc_array copy_relocations = {0,0};
    // For each relocation table, calculate its relocations.
    for (size_t index = 0; index < relocations.nRelocations; index++)
    {
        struct relocation_table table = relocations.buf[index];
        Elf32_Rel* relTable = (Elf32_Rel*)(base + table.table->d_un.d_ptr);
        Elf32_Rela* relaTable = (Elf32_Rela*)(base + table.table->d_un.d_ptr);
        for (size_t i = 0; i < table.nEntries; i++)  
        {
            struct relocation cur;
            if (!table.rel)
            {
                cur.symbolTableOffset = (uint32_t)(relaTable[i].r_info >> 8),
                cur.virtualAddress = relaTable[i].r_offset,
                cur.relocationType = (uint16_t)(relaTable[i].r_info & 0xff),
                cur.addend = relaTable[i].r_addend;
            }
            else
            {
                cur.symbolTableOffset = (uint32_t)(relTable[i].r_info >> 8),
                cur.virtualAddress = relTable[i].r_offset,
                cur.relocationType = (uint16_t)(relTable[i].r_info & 0xff),
                cur.addend = 0;
            }
            if (!calculate_relocation(status, 
                                      driver,
                                      symbolTable,
                                      stringTable,
                                      file, 
                                      cur, 
                                      base,
                                      szProgram,
                                      GOT,
                                      &copy_relocations,
                                      hashTableOffset))
                return nullptr;
        }
    }
    // TODO: Should I do this or not?
    GOT[0] += (uintptr_t)base;
    // GOT[0] = (uintptr_t)base + dynamic->p_vaddr;
    static uint32_t dummy;
    GOT[1] = (uintptr_t)&dummy;
    GOT[2] = (uintptr_t)dyn_loader_dummy;
    for (size_t i = 0; i < copy_relocations.nRelocations; i++)
    {
        struct copy_reloc reloc = copy_relocations.buf[i];
        memcpy(reloc.dest, reloc.src, reloc.size);
    }

    free_copy_reloc_array(&copy_relocations);
    free_reloc_array(&relocations);

    // Set protection.
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr_table[i].p_type != PT_LOAD)
            continue;
        if (!phdr_table[i].p_memsz)
            continue;
        void* phdrBase = OffsetPtr(base, phdr_table[i].p_vaddr, void*);
        phdrBase = (void*)((uintptr_t)phdrBase & ~0xfff);
        prot_flags prot = 0;
        bool isPageable = false;
        if (phdr_table[i].p_flags & PF_X)
            prot |= OBOS_PROTECTION_EXECUTABLE;
        if ((phdr_table[i].p_flags & PF_R) && !(phdr_table[i].p_flags & PF_W))
            prot |= OBOS_PROTECTION_READ_ONLY;
        if (phdr_table[i].p_flags & PF_OBOS_PAGEABLE)
            isPageable = true;
        else
            isPageable = false;
        Mm_VirtualMemoryProtect(&Mm_KernelContext, phdrBase, phdr_table[i].p_memsz, prot, isPageable);
    }
    if (status)
        *status = OBOS_STATUS_SUCCESS;
    if (dynamicSymbolTable)
        *dynamicSymbolTable = symbolTable;
    if (nEntriesDynamicSymbolTable)
    {
        if (!hashTableOffset)
        {
            // There is no possible way to know the size without a hash table.

            *nEntriesDynamicSymbolTable = 0;
        }
        else 
        {
            Elf32_Word* hashTable = OffsetPtr(base, hashTableOffset, Elf32_Word*);
            *nEntriesDynamicSymbolTable = hashTable[1];
        }
    }
    if (dynstrtab)
        *dynstrtab = OffsetPtr(base, stringTable, const char*);
    if (top)
        *top = OffsetPtr(base, end, void*);
    // Yes, most (nearly all) of the code in this file is stolen from the x86-64 implementation.
    return base;
}