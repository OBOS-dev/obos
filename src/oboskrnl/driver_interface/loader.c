/*
 * oboskrnl/driver_interface/loader.c
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <memmanip.h>
#include <stdint.h>
#include <struct_packing.h>

#include <utils/tree.h>

#include <scheduler/thread.h>
#include <scheduler/thread_context_info.h>
#include <scheduler/schedule.h>
#include <scheduler/process.h>
#include <scheduler/cpu_local.h>

#include <allocators/base.h>

#include <driver_interface/header.h>
#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>

#include <mm/alloc.h>
#include <mm/context.h>
#include <mm/page.h>

#include <irq/dpc.h>
#include <irq/irql.h>

#include <elf/elf.h>

// Do it in two passes so that any macros can be expanded
#define token_concat_impl(tok1, tok2) tok1 ##tok2
#define token_concat(tok1, tok2) token_concat_impl(tok1, tok2)
#define GetCurrentElfClass() token_concat(ELFCLASS, OBOS_ARCHITECTURE_BITS)

symbol_table OBOS_KernelSymbolTable;
driver_list Drv_LoadedDrivers;
driver_list Drv_LoadedFsDrivers;
static int cmp_symbols(driver_symbol* left, driver_symbol* right)
{
    return strcmp_std(left->name, right->name);
}
RB_GENERATE(symbol_table, driver_symbol, rb_entry, cmp_symbols);

#define OffsetPtr(ptr, off, type) ((type)(((uintptr_t)ptr) + ((intptr_t)off)))
// Please forgive me for this
#define Cast(what, to) ((to)what)

static uint32_t nextDriverId;
static OBOS_NO_UBSAN OBOS_NO_KASAN driver_header* find_header(void* file, size_t szFile)
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
static size_t get_header_size(driver_header* header)
{
    size_t sizeof_header = 0;
    if (~header->flags & DRIVER_HEADER_HAS_VERSION_FIELD)
        sizeof_header = sizeof(*header)-0x100; // An old-style driver.
    else
    {
        switch (header->version)
        {
            case CURRENT_DRIVER_HEADER_VERSION:
                sizeof_header = sizeof(*header);
                break;
#if CURRENT_DRIVER_HEADER_VERSION != 2
            case 2:
#endif
            case 1:
                sizeof_header = 928;
                break;
            default:
                return SIZE_MAX;
        }
    }
    return sizeof_header;
}
OBOS_NO_UBSAN obos_status Drv_LoadDriverHeader(const void* file_, size_t szFile, driver_header* header)
{
    if (!header)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (szFile < sizeof(Elf_Ehdr) || !file_)
        return OBOS_STATUS_INVALID_FILE;
    
    uint8_t* file = (uint8_t*)file_;
    Elf_Ehdr* ehdr = Cast(file, Elf_Ehdr*);
    if (ehdr->e_ident[0] != ELFMAG0 || ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 || ehdr->e_ident[3] != ELFMAG3
    )
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
    if (ehdr->e_type != ET_DYN)
        return OBOS_STATUS_INVALID_FILE;
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
    (void)0;
    driver_header* header_ = nullptr;
    if (!driverHeaderSection)
        goto manualSearch;
    header_ = OffsetPtr(ehdr, driverHeaderSection->sh_offset, driver_header*);
    manualSearch:
    if (!header_)
        header_ = find_header(file, szFile);
    if (!header_)
        return OBOS_STATUS_NOT_FOUND;
    // We've found the header.
    // Verify its contents.
    if (header_->magic != OBOS_DRIVER_MAGIC)
        return OBOS_STATUS_INVALID_HEADER;
    size_t sizeof_header = get_header_size(header_);
    if (sizeof_header == SIZE_MAX)
        return OBOS_STATUS_INVALID_HEADER;
    memcpy(header, header_, sizeof_header);
    memzero((void*)((uintptr_t)header + sizeof_header), sizeof(*header) - sizeof_header);
    return OBOS_STATUS_SUCCESS;
}
OBOS_NO_UBSAN driver_id *Drv_LoadDriver(const void* file_, size_t szFile, obos_status* status)
{
    driver_header* header;
    driver_header header_;
    obos_status st = Drv_LoadDriverHeader(file_, szFile, &header_);
    if (obos_is_error(st))
    {
        if (status)
            *status = st;
        return nullptr;
    }
    if (!header_.ftable.driver_cleanup_callback)
    {
        OBOS_Error("%s: Refusing to load a driver without a cleanup callback.\n", __func__);
        if (status) *status = OBOS_STATUS_INVALID_HEADER;
        return nullptr;
    }
    for (driver_node* curr = Drv_LoadedDrivers.head; curr; )
    {
        if (strncmp(header_.driverName, curr->data->header.driverName, 64))
        {
            OBOS_Error("%s: Refusing to load an already loaded driver.\n", __func__);
            if (status) *status = OBOS_STATUS_ALREADY_INITIALIZED;
            return nullptr;
        }
        curr = curr->next;
    }
    driver_id* driver = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_id), nullptr);
    Elf_Sym* dynamicSymbolTable = nullptr;
    size_t nEntriesDynamicSymbolTable = 0;
    const char* dynstrtab = nullptr;
    void* top = nullptr;
    // Temporarily do this.
    driver->header.flags = header_.flags;
    if (header_.flags & DRIVER_HEADER_HAS_VERSION_FIELD && header_.version >= 1)
        driver->header.uacpi_init_level_required = header_.uacpi_init_level_required;
    driver->base = DrvS_LoadRelocatableElf(driver, file_, szFile, &dynamicSymbolTable, &nEntriesDynamicSymbolTable, &dynstrtab, &top, status);
    if (!driver->base)
    {
        Free(OBOS_KernelAllocator, driver, sizeof(*driver));
        return nullptr;
    }
    Elf_Shdr* driverHeaderSection = nullptr;
    uint8_t* file = (uint8_t*)file_;
    Elf_Ehdr* ehdr = Cast(file, Elf_Ehdr*);
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
    driver->top = top;
    driver->id = nextDriverId++;
    driver->refCnt++;
    // Find the header within memory.
    {
        if (driverHeaderSection)
            header = OffsetPtr(driver->base, driverHeaderSection->sh_addr, driver_header*);
        else
        {
            header = find_header(driver->base, (uintptr_t)driver->top - (uintptr_t)driver->base);
            OBOS_ASSERT(header); // If this fails, something scuffed has happened.
        }
    }
    //driver->header = *header;
    size_t sizeof_header = get_header_size(header);
    memcpy(&driver->header, header, sizeof_header);
    memzero((void*)((uintptr_t)&driver->header + sizeof_header), sizeof(*header) - sizeof_header);
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
            "Drv_Header",
        };
        int symbolType = -1;
		switch (ELF_ST_TYPE(esymbol->st_info)) 
		{
			case STT_FUNC:
				symbolType = SYMBOL_TYPE_FUNCTION;
				break;
			case STT_FILE:
				symbolType = SYMBOL_TYPE_FILE;
				break;
			default:
				symbolType = SYMBOL_TYPE_VARIABLE;
				break;
		}
		driver_symbol* symbol = ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, sizeof(driver_symbol), nullptr);
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
		symbol->name = memcpy(ZeroAllocate(OBOS_NonPagedPoolAllocator, 1, szName + 1, nullptr), name, szName);
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
    driver->other_node.data = driver;
    if (driver->header.ftable.probe)
        APPEND_DRIVER_NODE(Drv_LoadedFsDrivers, &driver->other_node); // pretty high chance it is a fs driver
    if (strlen(driver->header.driverName))
        OBOS_Log("%s: Loaded driver '%s' at 0x%p.\n", __func__, driver->header.driverName, driver->base);
    else
        OBOS_Log("%s: Loaded driver at 0x%p.\n", __func__, driver->header.driverName, driver->base);
    driver->refCnt++;
    return driver;
}

typedef driver_init_status(*driver_entry)(driver_id* id);
__attribute__((no_instrument_function)) static void driver_trampoline(driver_id* id)
{
    OBOS_Debug("calling driver entry %p\n", id->entryAddr);
    driver_init_status status = ((driver_entry)id->entryAddr)(id);
    Drv_ExitDriver(id, &status);
}

obos_status Drv_StartDriver(driver_id* driver, thread** mainThread)
{
    if (mainThread)
        *mainThread = nullptr;
    if (!driver)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if (driver->header.flags & DRIVER_HEADER_FLAGS_NO_ENTRY)
        return OBOS_STATUS_NO_ENTRY_POINT;
    if (driver->started)
        return OBOS_STATUS_ALREADY_INITIALIZED;
    obos_status status = OBOS_STATUS_SUCCESS;
    thread* thr = CoreH_ThreadAllocate(&status);
    if (obos_is_error(status))
        return status;
    thread_ctx ctx;
    memzero(&ctx, sizeof(ctx));
    size_t stackSize = 0;
    if (driver->header.flags & DRIVER_HEADER_FLAGS_REQUEST_STACK_SIZE)
        stackSize = driver->header.stackSize;
    if (!stackSize)
        stackSize = 0x20000;
    void* stack = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, stackSize, 0, VMA_FLAGS_KERNEL_STACK, nullptr, &status);
    status = CoreS_SetupThreadContext(&ctx, 
        (uintptr_t)driver_trampoline,
        (uintptr_t)driver,
        false,
        stack,
        stackSize);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, stack, stackSize);
        return status;
    }
    status = CoreH_ThreadInitialize(thr, THREAD_PRIORITY_HIGH, !driver->header.mainThreadAffinity ? Core_DefaultThreadAffinity : driver->header.mainThreadAffinity, &ctx);
    if (obos_is_error(status))
    {
        Mm_VirtualMemoryFree(&Mm_KernelContext, stack, stackSize);
        return status;
    }
    Core_ProcessAppendThread(OBOS_KernelProcess, thr);
    thr->stackFree = CoreH_VMAStackFree;
    thr->stackFreeUserdata = &Mm_KernelContext;
    if (mainThread)
    {
        thr->references++;
        *mainThread = thr;
    }
    driver->main_thread = thr;
    thr->references++;
    driver->started = true;
    CoreH_ThreadReady(thr); // very low chance of failure.
    return OBOS_STATUS_SUCCESS;
}

obos_status Drv_UnloadDriver(driver_id* driver)
{
    // The kernel don't want the driver no more.
    --driver->refCnt;
    if (driver->refCnt != 1)
        OBOS_Warning("Driver not unloaded because refcount=%ld\n", driver->refCnt);
    return Drv_UnrefDriver(driver);
}

obos_status Drv_UnrefDriver(driver_id* driver)
{
    if (!driver)
        return OBOS_STATUS_INVALID_ARGUMENT;
    if ((--driver->refCnt) > 0)
        return OBOS_STATUS_SUCCESS;
    if (driver->main_thread)
    {
        while (!(driver->main_thread->flags & THREAD_FLAGS_DIED))
            Core_Yield();
        if (!(--driver->main_thread->references) && driver->main_thread->free)
            driver->main_thread->free(driver->main_thread);
        driver->main_thread = 0;
    }
    driver->header.ftable.driver_cleanup_callback();
    for (driver_node* node = driver->dependencies.head; node; )
    {
        // node->data->refCnt--;
        Drv_UnrefDriver(node->data);

        node = node->next;
    }
    REMOVE_DRIVER_NODE(Drv_LoadedDrivers, &driver->node);
    size_t size = ((uintptr_t)driver->top-(uintptr_t)driver->base);
    if (size % OBOS_PAGE_SIZE)
        size += (OBOS_PAGE_SIZE-(size%OBOS_PAGE_SIZE));
    // Mm_VirtualMemoryFree(&Mm_KernelContext, driver->base, size);
    Free(OBOS_KernelAllocator, driver, sizeof(*driver));
    return OBOS_STATUS_SUCCESS;
}

driver_symbol* DrvH_ResolveSymbol(const char* name, struct driver_id** driver)
{
    OBOS_ASSERT(driver);
    OBOS_ASSERT(name);
    OBOS_ASSERT(strlen(name));
    // Search the kernel symbol table;
    driver_symbol what = { .name = name };
    driver_symbol* sym = RB_FIND(symbol_table, &OBOS_KernelSymbolTable, &what);
    if (sym)
    {
        *driver = nullptr; // the kernel
        return sym;
    }
    for (driver_node* node = Drv_LoadedDrivers.head; node; )
    {
        driver_id* drv = node->data;
        OBOS_ASSERT(drv);
        sym = RB_FIND(symbol_table, &drv->symbols, &what);
        if (sym)
        {
            *driver = drv; // the current driver.
            return sym;
        }

        node = node->next;
    }
    *driver = nullptr;
    return nullptr; // symbol unresolved.
}

static __attribute__((no_instrument_function)) void unload_driver_dpc(dpc* unused, void* userdata)
{
    Drv_UnloadDriver(userdata);
    CoreH_FreeDPC(unused, true);
}

void Drv_ExitDriver(struct driver_id* id, const driver_init_status* status)
{
    if (!id || !status)
        return;
    if (id->main_thread != Core_GetCurrentThread())
        return;
    // NOTE: It is impossible for this to be zero after this, because
    // the main thread is referenced by the scheduler.
    id->main_thread->references--;
    id->main_thread = nullptr;
    if (obos_is_error(status->status))
    {
        if (OBOS_GetLogLevel() <= LOG_LEVEL_WARNING)
        {
            OBOS_Warning("Initialization of driver %d (%s) failed with status %d.\n", 
                id->id, 
                strnlen(id->header.driverName, 64) ? id->header.driverName : "Unknown",
                status->status
            );
            if (status->context)
                printf("Note: %s\n", status->context);
            if (status->fatal)
                printf("Note: Fatal error. Unloading the driver.\n");
        }
    }
    if (!status->fatal || obos_is_success(status->status))
        Core_ExitCurrentThread();
    dpc* dpc = CoreH_AllocateDPC(nullptr);
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);
    OBOS_ASSERT(dpc);
    dpc->userdata = id;
    CoreH_InitializeDPC(dpc, unload_driver_dpc, CoreH_CPUIdToAffinity(CoreS_GetCPULocalPtr()->id) /* make sure this runs on this CPU. */);
    Core_ExitCurrentThread();
    OBOS_UNREACHABLE;
    OBOS_UNUSED(oldIrql);
}

driver_symbol* DrvH_ResolveSymbolReverse(uintptr_t addr, struct driver_id** driver)
{
    OBOS_ASSERT(driver);
    if (addr < OBOS_KERNEL_ADDRESS_SPACE_BASE)
        goto unresolved;
    *driver = nullptr;
    driver_symbol* curr = nullptr;
    RB_FOREACH(curr, symbol_table, &OBOS_KernelSymbolTable)
    {
        if (addr >= curr->address && addr < (curr->address+curr->size))
            return curr;
    }
    for (driver_node* node = Drv_LoadedDrivers.head; node; )
    {
        driver_id* const drv = node->data;
        node = node->next;
        RB_FOREACH(curr, symbol_table, &drv->symbols)
        {
            if (addr >= curr->address && addr < (curr->address+curr->size))
            {
                *driver = drv;
                return curr;
            }
        }
    }
    unresolved:
    return nullptr;
}
