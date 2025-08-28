/*
 * oboskrnl/kinit.c
 *
 * Copyright (c) 2025 Omar Berrow
*/

#include <int.h>
#include <error.h>
#include <klog.h>
#include <kinit.h>
#include <init_proc.h>
#include <cmdline.h>
#include <memmanip.h>
#include <partition.h>

#include <irq/irq.h>
#include <irq/irql.h>
#include <irq/timer.h>

#include <mm/pmm.h>
#include <mm/swap.h>
#include <mm/initial_swap.h>
#include <mm/init.h>
#include <mm/alloc.h>

#include <scheduler/cpu_local.h>
#include <scheduler/process.h>
#include <scheduler/thread.h>
#include <scheduler/schedule.h>

#include <power/init.h>

#include <allocators/base.h>
#include <allocators/basic_allocator.h>

#include <driver_interface/loader.h>
#include <driver_interface/driverId.h>
#include <driver_interface/pnp.h>

#include <vfs/init.h>
#include <vfs/mount.h>
#include <vfs/dirent.h>
#include <vfs/vnode.h>
#include <vfs/fd.h>

#include <elf/elf.h>

allocator_info* OBOS_KernelAllocator;

#if OBOS_ARCHITECTURE_HAS_ACPI
#   include <uacpi/utilities.h>
#endif

static basic_allocator kalloc;
static struct boot_module initrd_drv_module = {};
static struct boot_module initrd_module = {};
static struct boot_module kernel_module = {};
static swap_dev swap;

static void get_initrd_module()
{
    char* initrd_module_name = OBOS_GetOPTS("initrd-module");
    char* initrd_driver_module_name = OBOS_GetOPTS("initrd-driver-module");
    if (initrd_module_name && initrd_driver_module_name)
    {
        OBOS_Debug("InitRD module name: %s, InitRD driver name: %s.\n", initrd_module_name, initrd_driver_module_name);
        OBOSS_GetModule(&initrd_drv_module, initrd_driver_module_name);
        OBOSS_GetModule(&initrd_module, initrd_module_name);
        if (!initrd_drv_module.address)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find module %s.\n", initrd_driver_module_name);
        if (!initrd_module.address)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not find module %s.\n", initrd_module_name);
        OBOS_InitrdBinary = (const char*)initrd_module.address;
        OBOS_InitrdSize = initrd_module.size;
        OBOS_Debug("InitRD is at %p (size: %d)\n", OBOS_InitrdBinary, OBOS_InitrdSize);
    }
    else
        OBOS_Warning("Could not find either 'initrd-module' or 'initrd-driver-module'. Kernel will run without an initrd.\n");
    if (initrd_module_name)
        Free(OBOS_KernelAllocator, initrd_module_name, strlen(initrd_module_name));
    if (initrd_driver_module_name)
        Free(OBOS_KernelAllocator, initrd_driver_module_name, strlen(initrd_driver_module_name));
}

static void foreach_string_in_list(const char* list, void(*cb)(const char* name, size_t name_len, void* userdata), void* userdata)
{
    size_t len = strlen(list);
    size_t left = len;
    const char* iter = list;
    while(iter < (list + len))
    {
        size_t namelen = strchr(iter, ',');
        if (namelen != left)
            namelen--;
        cb(iter, namelen, userdata);
        if (namelen != len)
            namelen++;
        iter += namelen;
        left -= namelen;
    }
}

static void load_driver_files(const char* name, size_t namelen, void* userdata)
{
    OBOS_UNUSED(userdata);

    OBOS_Debug("Loading driver %.*s.\n", namelen, name);
    char* path = memcpy(
        ZeroAllocate(OBOS_KernelAllocator, namelen+1, sizeof(char), nullptr),
        name,
        namelen
    );
    fd file = {};
    obos_status status = Vfs_FdOpen(&file, path, FD_OFLAGS_READ);
    Free(OBOS_KernelAllocator, path, namelen+1);
    if (obos_is_error(status))
    {
        OBOS_Warning("Could not load driver %.*s. Status: %d\n", namelen, name, status);
        return;
    }
    size_t filesize = file.vn->filesize;
    void *buff = Mm_VirtualMemoryAlloc(&Mm_KernelContext, nullptr, filesize, 0, VMA_FLAGS_PRIVATE, &file, &status);
    if (obos_is_error(status))
    {
        OBOS_Warning("Could not load driver %.*s. Status: %d\n", namelen, name, status);
        Vfs_FdClose(&file);
        return;
    }
    driver_id* drv = 
        Drv_LoadDriver(buff, filesize, &status);
    Mm_VirtualMemoryFree(&Mm_KernelContext, buff, filesize);
    Vfs_FdClose(&file);
    if (obos_is_error(status))
    {
        OBOS_Warning("Could not load driver %.*s. Status: %d\n", namelen, name, status);
        return;
    }
    
    thread* main = nullptr;
    status = Drv_StartDriver(drv, &main);
    if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
    {
        OBOS_Warning("Could not start driver %*s. Status: %d\n", namelen, name, status);
        status = Drv_UnloadDriver(drv);
        if (obos_is_error(status))
            OBOS_Warning("Could not unload driver %*s. Status: %d\n", namelen, name, status);
        return;
    }

    if (status != OBOS_STATUS_NO_ENTRY_POINT)
    {
        while (drv->main_thread)
            OBOSS_SpinlockHint();
    }

    return;
}

static void load_driver_modules(const char* name, size_t namelen, void* userdata)
{
    OBOS_UNUSED(userdata);

    obos_status status = OBOS_STATUS_SUCCESS;

    struct boot_module module = {};
    OBOSS_GetModuleLen(&module, name, namelen);
    if (module.is_kernel)
    {
        OBOS_Error("Cannot load the kernel as a driver.\n");
        return;
    }
    OBOS_Debug("Loading driver %.*s.\n", namelen, name);
    driver_id* drv = 
        Drv_LoadDriver((void*)module.address, module.size, &status);
    if (obos_is_error(status))
    {
        OBOS_Warning("Could not load driver %s. Status: %d\n", module.name, status);
        return;
    }
    status = Drv_StartDriver(drv, nullptr);
    if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
    {
        OBOS_Warning("Could not start driver %s. Status: %d\n", module.name, status);
        status = Drv_UnloadDriver(drv);
        if (obos_is_error(status))
            OBOS_Warning("Could not unload driver %s. Status: %d\n", module.name, status);
        return;
    }
    if (status != OBOS_STATUS_NO_ENTRY_POINT)
    {
        while (drv->main_thread)
            OBOSS_SpinlockHint();
    }
}

void OBOS_KernelInit()
{
    obos_status status = OBOS_STATUS_SUCCESS;
    irql oldIrql = Core_RaiseIrql(IRQL_DISPATCH);

    OBOSS_GetKernelModule(&kernel_module);

    OBOS_Debug("%s: Initializing PMM.\n", __func__);
    Mm_InitializePMM();
    if (OBOSS_KernelPostPMMInit)
        OBOSS_KernelPostPMMInit();
    
    OBOS_Debug("%s: Initializing allocator...\n", __func__);
    OBOSH_ConstructBasicAllocator(&kalloc);
    OBOS_KernelAllocator = (allocator_info*)&kalloc;
    get_initrd_module();

#if OBOS_ENABLE_PROFILING
    prof_start();
#endif
    
    OBOS_Debug("%s: Setting up uACPI early table access\n", __func__);
    OBOS_SetupEarlyTableAccess();
    
    OBOS_Debug("%s: Initializing kernel process.\n", __func__);
    OBOS_KernelProcess = Core_ProcessAllocate(&status);
    if (obos_is_error(status))
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not allocate a process object. Status: %d.\n", status);
    OBOS_KernelProcess->pid = Core_NextPID++;
    if (OBOSS_KernelPostKProcInit)
        OBOSS_KernelPostKProcInit();

    if (OBOSS_InitializeSMP)
        OBOSS_InitializeSMP();

    OBOS_Debug("%s: Initializing IRQ interface.\n", __func__);
    if (obos_is_error(status = Core_InitializeIRQInterface()))
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not initialize irq interface. Status: %d.\n", status);
    if (OBOSS_KernelPostIRQInit)
        OBOSS_KernelPostIRQInit();
    Core_LowerIrql(oldIrql);

    OBOS_Debug("%s: Initializing VMM.\n", __func__);
    Mm_InitializeInitialSwapDevice(&swap, OBOS_GetOPTD("initial-swap-size"));
    // We can reclaim the memory used.
    Mm_SwapProvider = &swap;
    Mm_Initialize();
    if (OBOSS_KernelPostVMMInit)
        OBOSS_KernelPostVMMInit();

    OBOS_Debug("%s: Initializing timer interface.\n", __func__);
    Core_InitializeTimerInterface();
    OBOS_Debug("%s: Initializing PCI bus 0\n\n", __func__);
    Drv_EarlyPCIInitialize();
    OBOS_Log("%s: Initializing uACPI\n", __func__);
    OBOS_InitializeUACPI();
    OBOS_Debug("%s: Initializing other PCI buses\n\n", __func__);
    Drv_PCIInitialize();

#if OBOS_ARCHITECTURE_HAS_ACPI
    // Set the interrupt model.
    uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
#endif

    OBOS_LoadSymbolTable();

    if (initrd_drv_module.address)
    {
        OBOS_Log("Loading InitRD driver.\n");
        // Load the InitRD driver.
        status = OBOS_STATUS_SUCCESS;
        driver_id* drv = 
            Drv_LoadDriver((void*)initrd_drv_module.address, initrd_drv_module.size, &status);
        if (obos_is_error(status))
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not load the InitRD driver passed in module %s.\nStatus: %d.\n", initrd_drv_module.name, status);
        thread* main = nullptr;
        status = Drv_StartDriver(drv, &main);
        if (obos_is_error(status) && status != OBOS_STATUS_NO_ENTRY_POINT)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Could not start the InitRD driver passed in module %s.\nStatus: %d.\nNote: This is a bug, please report it.\n", initrd_drv_module.name, status);
        if (status != OBOS_STATUS_NO_ENTRY_POINT)
        {
            while (drv->main_thread)
                OBOSS_SpinlockHint();
        }
        OBOS_Log("Loaded InitRD driver.\n");
        OBOS_Debug("%s: Initializing VFS.\n", __func__);
        Vfs_Initialize();
    }
    else 
    {
        OBOS_Debug("%s: Initializing VFS.\n", __func__);
        Vfs_Initialize();
        OBOS_Debug("No InitRD driver!\n");
        OBOS_Debug("Scanning command line...\n");
        char* modules_to_load = OBOS_GetOPTS("load-modules");
        if (!modules_to_load)
            OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "No initrd, and no drivers passed via the command line. Further boot is impossible.\n");
        foreach_string_in_list(modules_to_load, load_driver_modules, nullptr);
    }

    OBOS_Log("%s: Loading drivers through PnP.\n", __func__);
    Drv_PnpLoadDriversAt(Vfs_Root, true);
    do {
        if (!initrd_drv_module.address)
            break;
        char* modules_to_load = OBOS_GetOPTS("load-modules");
        if (!modules_to_load)
            break;
        foreach_string_in_list(modules_to_load, load_driver_files, nullptr);
    } while(0);
    if (Drv_PnpLoad_uHDA() == OBOS_STATUS_SUCCESS)
        OBOS_Log("Initialized HDA devices via %s\n", OBOS_ENABLE_UHDA ? "uHDA" : nullptr /* should be impossible */);

    OBOS_Log("%s: Probing partitions.\n", __func__);
    OBOS_PartProbeAllDrives(true);
    
    OBOS_Debug("%s: Finalizing VFS initialization...\n", __func__);
    Vfs_FinalizeInitialization();

    if (OBOSS_MakeTTY)
        OBOSS_MakeTTY();
    
    OBOS_LoadInit();

    OBOS_Log("%s: Done early boot.\n", __func__);
    OBOS_Log("Currently at %ld KiB of committed memory (%ld KiB pageable), %ld KiB paged out, %ld KiB non-paged, and %ld KiB uncommitted. %ld KiB of physical memory in use. Page faulted %ld times (%ld hard, %ld soft).\n", 
        Mm_KernelContext.stat.committedMemory/0x400,
        Mm_KernelContext.stat.pageable/0x400,
        Mm_KernelContext.stat.paged/0x400,
        Mm_KernelContext.stat.nonPaged/0x400,
        Mm_KernelContext.stat.reserved/0x400,
        Mm_PhysicalMemoryUsage/0x400,
        Mm_KernelContext.stat.pageFaultCount,
        Mm_KernelContext.stat.hardPageFaultCount,
        Mm_KernelContext.stat.softPageFaultCount
    );
#if OBOS_ENABLE_PROFILING
    prof_stop();
    prof_show("oboskrnl");
#endif
}

void OBOS_LoadSymbolTable()
{
    OBOS_Debug("%s: Loading kernel symbol table.\n", __func__);
    Elf_Ehdr* ehdr = (Elf_Ehdr*)kernel_module.address;
    Elf_Shdr* sectionTable = (Elf_Shdr*)(kernel_module.address + ehdr->e_shoff);
    if (!sectionTable)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the section table from oboskrnl.\n");
    const char* shstr_table = (const char*)(kernel_module.address + (sectionTable + ehdr->e_shstrndx)->sh_offset);
    // Look for .symtab
    Elf_Shdr* symtab = nullptr;
    const char* strtable = nullptr;
    for (size_t i = 0; i < ehdr->e_shnum; i++)
    {
        const char* section = shstr_table + sectionTable[i].sh_name;
        if (strcmp(section, ".symtab"))
            symtab = &sectionTable[i];
        if (strcmp(section, ".strtab"))
            strtable = (const char*)(kernel_module.address + sectionTable[i].sh_offset);
        if (strtable && symtab)
            break;
    }
    if (!symtab)
        OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Do not strip the symbol table from oboskrnl.\n");
    Elf_Sym* symbolTable = (Elf_Sym*)(kernel_module.address + symtab->sh_offset);
    for (size_t i = 0; i < symtab->sh_size/sizeof(Elf_Sym); i++)
    {
        Elf_Sym* esymbol = &symbolTable[i];
        int symbolType = -1;
        switch (ELF_ST_TYPE(esymbol->st_info)) 
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
        driver_symbol* symbol = ZeroAllocate(OBOS_KernelAllocator, 1, sizeof(driver_symbol), nullptr);
        const char* name = strtable + esymbol->st_name;
        size_t szName = strlen(name);
        symbol->name = memcpy(ZeroAllocate(OBOS_KernelAllocator, 1, szName + 1, nullptr), name, szName);
        symbol->address = esymbol->st_value;
        symbol->size = esymbol->st_size;
        symbol->type = symbolType;
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
                OBOS_Panic(OBOS_PANIC_FATAL_ERROR, "Unrecognized visibility %d.\n", esymbol->st_other);
        }
        RB_INSERT(symbol_table, &OBOS_KernelSymbolTable, symbol);
    }
}