/*
 * oboskrnl/driver_interface/loader.c
 *
 * Copyright (c) 2024 Omar Berrow
*/

#include "allocators/base.h"
#include "elf/elf64.h"
#include "klog.h"
#include "mm/alloc.h"
#include "mm/context.h"
#include "scheduler/thread.h"
#include "scheduler/thread_context_info.h"
#include <int.h>
#include <error.h>
#include <memmanip.h>
#include <struct_packing.h>

#include <stdint.h>
#include <utils/tree.h>

#include <driver_interface/header.h>
#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>

#if OBOS_ARCHITECTURE_BITS == 64
#   include <elf/elf.h>
#endif

// Do it in two passes so that any macros can be expanded
#define token_concat_impl(tok1, tok2) tok1 ##tok2
#define token_concat(tok1, tok2) token_concat_impl(tok1, tok2)
#define GetCurrentElfClass() token_concat(ELFCLASS, OBOS_ARCHITECTURE_BITS)

symbol_table OBOS_KernelSymbolTable;
driver_list Drv_LoadedDrivers;
RB_GENERATE(symbol_table, driver_symbol, rb_entry, cmp_symbols);

#define OffsetPtr(ptr, off, type) ((type)(((uintptr_t)ptr) + ((intptr_t)off)))
// Please forgive me for this
#define Cast(what, to) ((to)what)

static uint32_t nextDriverId;
static OBOS_NO_UBSAN driver_header* find_header(void* file, size_t szFile)
{
    for (driver_header* curr = Cast(file, driver_header*); 
            Cast(curr, uintptr_t) < (Cast(file, uintptr_t) + szFile); 
            curr = OffsetPtr(curr, 0x8, driver_header*))
    {
        if (curr->magic == OBOS_DRIVER_MAGIC)
            return curr;
    }
    return nullptr;
}
driver_id *Drv_LoadDriver(void* file_, size_t szFile, obos_status* status)
{
    if (status) *status = OBOS_STATUS_SUCCESS;
    if (szFile < sizeof(Elf_Ehdr))
    {
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    uint8_t* file = (uint8_t*)file_;
    Elf_Ehdr* ehdr = Cast(file, Elf_Ehdr*);
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3
    )
    {
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    if (ehdr->e_ident[EI_CLASS] != GetCurrentElfClass())
    {
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    uint8_t ei_data_val = 0;
	if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Little-Endian"))
		ei_data_val = ELFDATA2LSB;
	else if (strcmp(OBOS_ARCHITECTURE_ENDIANNESS, "Big-Endian"))
		ei_data_val = ELFDATA2MSB;
	else
		ei_data_val = ELFDATANONE;
    if (ehdr->e_ident[EI_DATA] != ei_data_val)
	{
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    if (ehdr->e_machine != EM_CURRENT)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    if (ehdr->e_type != ET_DYN)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_FILE;
        return nullptr;
    }
    // First, search for the section containing the driver header.
    Elf_Shdr* driverHeaderSection = nullptr;
    {
        Elf_Shdr* sectionTable = OffsetPtr(ehdr, ehdr->e_shoff, Elf_Shdr*);
        if (!sectionTable)
        {
            // No section table!
            // Skip.
            goto noSectionTable;
        }
        const char* shstr_table = (const char*)(((uintptr_t)file) + (sectionTable + ehdr->e_shstrndx)->sh_offset);
        for (size_t i = 0; i < ehdr->e_shnum; i++)
        {
            const char* section = shstr_table + sectionTable[i].sh_name;
            if (strcmp(section, OBOS_DRIVER_HEADER_SECTION))
                driverHeaderSection = &sectionTable[i];
            if (driverHeaderSection)
                break;
        }
    }
	noSectionTable:
    driver_header* header = nullptr;
    if (!driverHeaderSection)
        goto manualSearch;
    header = OffsetPtr(ehdr, driverHeaderSection->sh_offset, driver_header*);
    manualSearch:
    if (!header)
        header = find_header(file, szFile);
    if (!header)
    {
        if (status)
            *status = OBOS_STATUS_NOT_FOUND;
        return nullptr;
    }
    // We've found the header.
    // Verify its contents.
    if (header->magic != OBOS_DRIVER_MAGIC)
    {
        if (status)
            *status = OBOS_STATUS_INVALID_HEADER;
        return nullptr;
    }
    driver_id* driver = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_id), nullptr);
    Elf_Sym* dynamicSymbolTable = nullptr;
    size_t nEntriesDynamicSymbolTable = 0;
    const char* dynstrtab = nullptr;
    driver->base = DrvS_LoadRelocatableElf(driver, file, szFile, &dynamicSymbolTable, &nEntriesDynamicSymbolTable, &dynstrtab, status);
    if (!driver->base)
    {
        OBOS_KernelAllocator->Free(OBOS_KernelAllocator, driver, sizeof(*driver));
        return nullptr;
    }
    driver->id = nextDriverId++;
    driver->refCnt++;
    if (!(header->flags & DRIVER_HEADER_FLAGS_NO_ENTRY))
        driver->entryAddr = OffsetPtr(driver->base, ehdr->e_entry, uintptr_t);
    for (size_t i = 0; i < nEntriesDynamicSymbolTable; i++)
    {
        Elf_Sym* esymbol = &dynamicSymbolTable[i];
        if (esymbol->st_shndx == 0)
            continue; // Undefined symbol, do not add to the symbol list.
        const char* forced_hidden_symbols[] = {
            "OBOS_DriverEntry",
            "Drv_Base",
            "Drv_Top",
        };
        int symbolType = -1;
		switch (ELF64_ST_TYPE(esymbol->st_info)) 
		{
			case STT_FUNC:
				symbolType = SYMBOL_TYPE_FUNCTION;
				break;
			case STT_FILE:
				symbolType = SYMBOL_TYPE_FILE;
				break;
			case STT_OBJECT:
				symbolType = SYMBOL_TYPE_VARIABLE;
				break;
			default:
				continue;
		}
		driver_symbol* symbol = OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_symbol), nullptr);
		const char* name = dynstrtab + esymbol->st_name;
        bool forceHidden = false;
        for (size_t j = 0; j < sizeof(forced_hidden_symbols)/sizeof(*forced_hidden_symbols); j++)
        {
            if (strcmp(forced_hidden_symbols[j], name))
            {
                forceHidden = true;
                break;
            }
        }
		size_t szName = strlen(name);
		symbol->name = memcpy(OBOS_KernelAllocator->ZeroAllocate(OBOS_KernelAllocator, 1, szName + 1, nullptr), name, szName);
		symbol->address = OffsetPtr(driver->base, esymbol->st_value, uintptr_t);
		symbol->size = esymbol->st_size;
		symbol->type = symbolType;
        if (forceHidden)
        {
            symbol->visibility = SYMBOL_VISIBILITY_HIDDEN;
            continue;
        }
		switch (esymbol->st_other)
		{
			case STV_DEFAULT:
			case STV_EXPORTED:
			// since this is the kernel, everyone already gets the same object
			case STV_SINGLETON: 
				symbol->visibility = SYMBOL_VISIBILITY_DEFAULT;
				break;
			case STV_PROTECTED:
			case STV_HIDDEN:
				symbol->visibility = SYMBOL_VISIBILITY_HIDDEN;
				break;
			default:
				OBOS_Debug("%s: Unrecognized visibility %d. Assuming hidden.\n", __func__, esymbol->st_other);
				symbol->visibility = SYMBOL_VISIBILITY_HIDDEN;
				break;
		}
        RB_INSERT(symbol_table, &driver->symbols, symbol);
    }
    driver->node.data = driver;
    APPEND_DRIVER_NODE(Drv_LoadedDrivers, &driver->node);
    return driver;
}
obos_status Drv_StartDriver(driver_id* driver, thread** mainThread)
{
    if (mainThread)
        *mainThread = nullptr;
    if (!driver)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (driver->header.flags & DRIVER_HEADER_FLAGS_NO_ENTRY)
        return OBOS_STATUS_NO_ENTRY_POINT;
    obos_status status = OBOS_STATUS_SUCCESS;
    thread* thr = CoreH_ThreadAllocate(&status);
    if (obos_likely_error(status))
        return status;
    thread_ctx ctx;
    memzero(&ctx, sizeof(ctx));
    size_t stackSize = 0;
    if (driver->header.flags & DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE)
        stackSize = driver->header.stackSize;
    if (!stackSize)
        stackSize = 0x20000;
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, stackSize, 0, VMA_FLAGS_KERNEL_STACK, &status);
    status = CoreS_SetupThreadContext(&ctx, 
        driver->entryAddr,
        (uintptr_t)driver,
        false,
        stack,
        stackSize);
    if (obos_likely_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, stack, stackSize);
        return status;
    }
    status = CoreH_ThreadInitialize(thr, THREAD_PRIORITY_HIGH, Core_DefaultThreadAffinity, &ctx);
    if (obos_likely_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, stack, stackSize);
        return status;
    }
    CoreH_ThreadReady(thr); // very low chance of failure.
    if (mainThread)
        *mainThread = thr;
    return OBOS_STATUS_SUCCESS;
}

driver_symbol* DrvH_ResolveSymbol(const char* name, struct driver_id** driver)
{
    OBOS_ASSERT(driver);
    OBOS_ASSERT(name);
    OBOS_ASSERT(strlen(name));
    // Search the kernel symbol table;
    driver_symbol what = { .name = name };
    driver_symbol* sym = RB_FIND(symbol_table, &OBOS_KernelSymbolTable, &what );
    if (sym)
    {
        *driver = nullptr; // the kernel
        return sym;
    }
    for (driver_node* node = Drv_LoadedDrivers.head; node; )
    {
        driver_id* drv = node->data;
        OBOS_ASSERT(drv);
        sym = RB_FIND(symbol_table, &drv->symbols, &what );
        if (sym)
        {
            OBOS_Debug("Found symbol %s in driver %d.\n", name, drv->id);
            *driver = drv; // the current driver.
            return sym;
        }

        node = node->next;
    }
    *driver = nullptr;
    return nullptr; // symbol unresolved.
}