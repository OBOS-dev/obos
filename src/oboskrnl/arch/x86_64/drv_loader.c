/*
 * oboskrnl/arch/x86-_64/drv_loader.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>
#include <error.h>
#include <memmanip.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#include <mm/alloc.h>
#include <mm/context.h>

#include <driver_interface/loader.h>
#include <driver_interface/header.h>
#include <driver_interface/driverId.h>

#include <elf/elf.h>

#include <allocators/base.h>

#define OffsetPtr(ptr, off, type) (type)(((uintptr_t)ptr) + ((intptr_t)off))
// Please forgive me for this
#define Cast(what, to) ((to)what)

// Helpers.
struct relocation_table
{
    Elf64_Dyn* table;
    size_t sz;
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
    OBOS_ASSERT(arr->buf); // oopsies
    arr->buf[arr->nRelocations - 1] = *what; 
    // Note:
    // Since you'll probably forget what you were doing:
    // You were making relocation tables in the array be stored as the struct relocation_table instead of Elf64_Dyn.
    // You're done adapting the array, you only need to adapt the code.
    // 5 Hours later:
    // thank god I wrote this comment.
}
static void free_reloc_array(struct relocation_array* arr)
{
    OBOS_KernelAllocator->Free(OBOS_KernelAllocator, arr->buf, arr->nRelocations*sizeof(*arr->buf));
    memzero(arr, sizeof(*arr));
}
static void append_copy_reloc(struct copy_reloc_array* arr, const struct copy_reloc* what)
{
    arr->buf = OBOS_KernelAllocator->Reallocate(OBOS_KernelAllocator, arr->buf, (++arr->nRelocations)*sizeof(*arr->buf), nullptr);
    OBOS_ASSERT(arr->buf); // oopsies
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
static Elf64_Sym* GetSymbolFromTable(
			uint8_t* fileStart,
			uint8_t* baseAddress,
			Elf64_Sym* symbolTable,
			uintptr_t hashTableOff,
			Elf64_Off stringTable,
			const char* _symbol)
{
    Elf64_Word* hashTableBase = (Elf64_Word*)(baseAddress + hashTableOff);
    Elf64_Word nBuckets = hashTableBase[0];
    uint32_t currentBucket = ElfHash(_symbol) % nBuckets;
    Elf64_Word* firstBucket = hashTableBase + 2;
    Elf64_Word* firstChain = firstBucket + nBuckets;
    size_t index = firstBucket[currentBucket];
    while (index)
    {
        Elf64_Sym* symbol = &symbolTable[index];
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
static bool calculate_relocation(obos_status* status, driver_id* drv, Elf64_Sym* symbolTable, Elf64_Off stringTable, void* file, struct relocation i, void* base, size_t szProgram, Elf64_Addr* GOT, struct copy_reloc_array* copy_relocations, uintptr_t hashTableOffset)
{
    driver_symbol* Symbol;
    driver_symbol internal_symbol = {};
    if (i.symbolTableOffset)
    {
        Elf64_Sym* Unresolved_Symbol = &symbolTable[i.symbolTableOffset];
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
            Elf64_Sym* sym = GetSymbolFromTable(
                file, base, symbolTable, hashTableOffset, stringTable,
                OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*)
            );
            if (sym->st_shndx == 0)
                goto unresolved;
            internal_symbol.address = OffsetPtr(base, sym->st_value, uintptr_t);
            internal_symbol.size = sym->st_size;
            internal_symbol.visibility = SYMBOL_VISIBILITY_DEFAULT;
            switch (ELF64_ST_TYPE(sym->st_info)) 
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
        if (!Symbol) 
        {
            if (status)
                *status = OBOS_STATUS_DRIVER_REFERENCED_UNRESOLVED_SYMBOL;
            OBOS_Debug("Could not resolve symbol '%s' referenced within a driver.\n", OffsetPtr(base, stringTable + Unresolved_Symbol->st_name, const char*));
            Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
            return false;
        }
        add_dependency(drv, dependency);
        // TODO: add driver dependency to list.
        if (Unresolved_Symbol->st_size != Symbol->size && i.relocationType == R_AMD64_COPY)
        {
            // Oh no!
            if (status)
                *status = OBOS_STATUS_DRIVER_SYMBOL_MISMATCH;
            Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
            return false;
        }
    }
    int type = i.relocationType;
    uintptr_t relocAddr = (uintptr_t)base + i.virtualAddress;
    uint64_t relocResult = 0;
    uint8_t relocSize = 0;
    switch (type)
    {
    case R_AMD64_NONE:
        return true;
    case R_AMD64_64:
        relocResult = Symbol->address + i.addend;
        relocSize = 8;
        break;
    case R_AMD64_PC32:
        relocResult = Symbol->address + i.addend - relocAddr;
        relocSize = 4;
        break;
    case R_AMD64_GOT32:
        if (status)
            *status = OBOS_STATUS_UNIMPLEMENTED;
        return false;
        // TODO: Replace the zero in the calculation with "G" (see elf spec for more info).
        relocResult = 0 + i.addend;
        relocSize = 4;
        break;
    case R_AMD64_PLT32:
        if (status)
            *status = OBOS_STATUS_UNIMPLEMENTED;
        Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
        return false;
        // TODO: Replace the zero in the calculation with "L" (see elf spec for more info).
        relocResult = 0 + i.addend - relocAddr;
        relocSize = 4;
        break;
    case R_AMD64_COPY:
    {
        // Save copy relocations for the end because if we don't, it might contain unresolved addresses.
        // copy_relocations.push_back({ (void*)relocAddr, (void*)Symbol->address, Symbol->size });
        struct copy_reloc reloc = { (void*)relocAddr, (void*)Symbol->address, Symbol->size };
        append_copy_reloc(copy_relocations, &reloc);
        relocSize = 0;
        break;
    }
    case R_AMD64_JUMP_SLOT:
    case R_AMD64_GLOB_DAT:
        relocResult = Symbol->address;
        relocSize = 8;
        break;
    case R_AMD64_RELATIVE:
        relocResult = (uint64_t)base + i.addend;
        relocSize = 8;
        break;
    case R_AMD64_GOTPCREL:
        if (status)
            *status = OBOS_STATUS_UNIMPLEMENTED;
        Mm_VirtualMemoryFree(&Mm_KernelContext, base, szProgram);
        return false;
        // TODO: Replace the zero in the calculation with "G" (see elf spec for more info).
        relocResult = 0 + (uint64_t)base + i.addend - relocAddr;
        relocSize = 8;
        break;
    case R_AMD64_32:
        relocResult = Symbol->address + i.addend;
        relocSize = 4;
        break;
    case R_AMD64_32S:
        relocResult = Symbol->address + i.addend;
        relocSize = 4;
        break;
    case R_AMD64_16:
        relocResult = Symbol->address + i.addend;
        relocSize = 2;
        break;
    case R_AMD64_PC16:
        relocResult = Symbol->address + i.addend - relocAddr;
        relocSize = 2;
        break;
    case R_AMD64_8:
        relocResult = Symbol->address + i.addend;
        relocSize = 1;
        break;
    case R_AMD64_PC8:
        relocResult = Symbol->address + i.addend - relocAddr;
        relocSize = 1;
        break;
    case R_AMD64_PC64:
        relocResult = Symbol->address + i.addend - relocAddr;
        relocSize = 8;
        break;
    case R_AMD64_GOTOFF64:
        relocResult = Symbol->address + i.addend - ((uint64_t)GOT);
        relocSize = 8;
        break;
    case R_AMD64_GOTPC32:
        relocResult = (uint64_t)GOT + i.addend + relocAddr;
        relocSize = 8;
        break;
    case R_AMD64_SIZE32:
        relocSize = 4;
        relocResult = Symbol->size + i.addend;
        break;
    case R_AMD64_SIZE64:
        relocSize = 8;
        relocResult = Symbol->size + i.addend;
        break;
    default:
        break; // Ignore.
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
        *(uint32_t*)(relocAddr) = (uint16_t)(relocResult & 0xffffffff);
        break;
    case 8: // word64
        *(uint64_t*)(relocAddr) = relocResult;
        break;
    default:
        break;
    }
    return true;
}

void* DrvS_LoadRelocatableElf(driver_id* driver, void* file, size_t szFile, Elf_Sym** dynamicSymbolTable, size_t* nEntriesDynamicSymbolTable, const char** dynstrtab, void** top, obos_status* status)
{
    Elf64_Ehdr* ehdr = Cast(file, Elf64_Ehdr*);
    Elf64_Phdr* phdr_table = OffsetPtr(ehdr, ehdr->e_phoff, Elf64_Phdr*);
    Elf64_Phdr* dynamic = nullptr;
    size_t szProgram = 0;
    void* end = nullptr;
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr_table[i].p_type == PT_DYNAMIC)
            dynamic = &phdr_table[i];
        if (phdr_table[i].p_type != PT_LOAD)
            continue;
        Elf64_Phdr* curr = &phdr_table[i];
        if (!end || curr->p_vaddr > (uintptr_t)end)
			end = (void*)(curr->p_vaddr+curr->p_memsz);
    }
    szProgram = (size_t)end;
    void* base = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, szProgram, 0, VMA_FLAGS_GUARD_PAGE, status);
    if (!base)
        return nullptr;
    // The pages are mapped in.
    // Copy the phdr data into them.
    for (size_t i = 0; i < ehdr->e_phnum; i++)
    {
        if (phdr_table[i].p_type != PT_LOAD)
            continue;
        Elf64_Phdr* curr = &phdr_table[i];
        memcpy(OffsetPtr(base, curr->p_vaddr, void*), OffsetPtr(file, curr->p_offset, void*), curr->p_filesz);
        // NOTE: Possible buffer overflow
        memzero(OffsetPtr(base, curr->p_vaddr + curr->p_offset, void*), curr->p_memsz - curr->p_filesz); 
    }
    // Apply relocations.
    struct relocation_array relocations = {.buf=nullptr,.nRelocations=0};
    Elf64_Sym* symbolTable = 0;
	Elf64_Off stringTable = 0;
	uintptr_t hashTableOffset = 0;
	Elf64_Addr* GOT = nullptr;
    Elf64_Dyn* currentDynamicHeader = OffsetPtr(file, dynamic->p_offset, Elf64_Dyn*);
    size_t last_dtrelasz = 0, last_dtrelsz = 0, last_dtpltrelsz = 0;
    bool awaitingRelaSz = false, foundRelaSz = false;
    Elf64_Dyn* dynEntryAwaitingRelaSz = nullptr;
    bool awaitingRelSz = false, foundRelSz = false;
    Elf64_Dyn* dynEntryAwaitingRelSz = nullptr;
    uint64_t last_dlpltrel = 0;
    struct relocation_table current = {};
	for (size_t i = 0; currentDynamicHeader->d_tag != DT_NULL; i++, currentDynamicHeader++)
	{
        switch (currentDynamicHeader->d_tag)
        {
        case DT_HASH:
            hashTableOffset = currentDynamicHeader->d_un.d_ptr;
            break;
        case DT_PLTGOT:
            // TODO: Find out whether this is the PLT or GOT (if possible).
            GOT = (Elf64_Addr*)(base + currentDynamicHeader->d_un.d_ptr);
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
            current.sz = last_dtrelsz;
            append_relocation_table(&relocations, &current);
            foundRelSz = false;
            last_dtrelsz = 0;
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
            current.sz = last_dtrelsz;
            append_relocation_table(&relocations, &current);
            foundRelaSz = false;
            last_dtrelasz = 0;
            break;
        case DT_JMPREL:
            switch (last_dlpltrel)
            {
            case DT_REL:
                current.rel = true;
                current.table = currentDynamicHeader;
                current.sz = last_dtpltrelsz;
                append_relocation_table(&relocations, &current);
                break;
            case DT_RELA:
                current.rel = false;
                current.table = currentDynamicHeader;
                current.sz = last_dtpltrelsz;
                append_relocation_table(&relocations, &current);
                break;
            default:
                break;
            }
            break;
        case DT_RELSZ:
            last_dtrelsz = currentDynamicHeader->d_un.d_val;
            foundRelSz = !awaitingRelSz;
            if (awaitingRelSz)
            {
                current.rel = true;
                current.table = dynEntryAwaitingRelSz;
                current.sz = last_dtrelsz;
                append_relocation_table(&relocations, &current);
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
                current.sz = last_dtrelsz;
                append_relocation_table(&relocations, &current);
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
            symbolTable = (Elf64_Sym*)(base + currentDynamicHeader->d_un.d_ptr);
            break;
        default:
            break;
        }
    }

    struct copy_reloc_array copy_relocations = {0,0};
    for (size_t index = 0; index < relocations.nRelocations; index++)
    {
        struct relocation_table table = relocations.buf[index];
        Elf64_Rel* relTable = (Elf64_Rel*)(base + table.table->d_un.d_ptr);
        Elf64_Rela* relaTable = (Elf64_Rela*)(base + table.table->d_un.d_ptr);
        for (size_t i = 0; i < table.sz / (table.rel ? sizeof(Elf64_Rel) : sizeof(Elf64_Rela)); i++)  
        {
            struct relocation cur;
            if (!table.rel)
            {
                cur.symbolTableOffset = (uint32_t)(relaTable[i].r_info >> 32),
                cur.virtualAddress = relaTable[i].r_offset,
                cur.relocationType = (uint16_t)(relaTable[i].r_info & 0xffff),
                cur.addend = relaTable[i].r_addend;
            }
            else
            {
                cur.symbolTableOffset = (uint32_t)(relTable[i].r_info >> 32),
                cur.virtualAddress = relTable[i].r_offset,
                cur.relocationType = (uint16_t)(relTable[i].r_info & 0xffff),
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
                return nullptr; // the function frees the driver on its own.
        }
    }
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
            Elf64_Word* hashTable = OffsetPtr(base, hashTableOffset, Elf64_Word*);
            *nEntriesDynamicSymbolTable = hashTable[1];
        }
    }
    if (dynstrtab)
        *dynstrtab = OffsetPtr(base, stringTable, const char*);
    if (top)
        *top = OffsetPtr(base, top, void*);
    return base;
}