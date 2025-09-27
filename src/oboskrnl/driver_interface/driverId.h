/*
 * oboskrnl/driver_interface/driverId.h
 *
 * Copyright (c) 2024-2025 Omar Berrow
*/

#pragma once

#include <int.h>

#include <utils/tree.h>

#include <driver_interface/header.h>

#include <scheduler/thread.h>

enum
{
    SYMBOL_TYPE_FUNCTION,
    SYMBOL_TYPE_VARIABLE,
    SYMBOL_TYPE_FILE,
};
enum
{
    SYMBOL_VISIBILITY_DEFAULT,
    SYMBOL_VISIBILITY_HIDDEN,
};
typedef struct driver_symbol
{
    RB_ENTRY(driver_symbol) rb_entry;
    const char* name;
    uintptr_t address;
    uintptr_t size;
    int8_t type : 4;
    int8_t visibility : 4;
} driver_symbol;
typedef RB_HEAD(symbol_table, driver_symbol) symbol_table;
RB_PROTOTYPE(symbol_table, driver_symbol, rb_entry, cmp_symbols);
typedef struct driver_node
{
    struct driver_node *next, *prev;
    struct driver_id* data;
} driver_node;
typedef struct driver_list
{
    driver_node *head, *tail;
    size_t nNodes;
} driver_list;

// TODO: Lock?

#define APPEND_DRIVER_NODE(list, node) do {\
	(node)->next = nullptr;\
	(node)->prev = nullptr;\
	if ((list).tail)\
		(list).tail->next = (node);\
	if (!(list).head)\
		(list).head = (node);\
	(node)->prev = ((list).tail);\
	(list).tail = (node);\
	(list).nNodes++;\
} while(0)
#define REMOVE_DRIVER_NODE(list, node) do {\
	if ((list).tail == (node))\
		(list).tail = (node)->prev;\
	if ((list).head == (node))\
		(list).head = (node)->next;\
	if ((node)->prev)\
		(node)->prev->next = (node)->next;\
	if ((node)->next)\
		(node)->next->prev = (node)->prev;\
	(list).nNodes--;\
} while(0)

typedef struct driver_id
{
    uint32_t id;
    void* base;
    void* top;
    symbol_table symbols;
    // A copy of the driver's header.
    driver_header header;
    uintptr_t entryAddr; // If zero, there is no entry point.
    // The amount of loaded drivers that depend on this driver.
    // Also incremented whenever a user thread opens a handle to this driver.
    // This is set to one (the kernel) on driver load.
    size_t refCnt;
    // The driver's dependencies.
    driver_list dependencies;
    // The node in Drv_LoadedDrivers
    driver_node node;
    // The node in Drv_LoadedFsDrivers
    driver_node other_node;
    thread* main_thread;
    bool started;
} driver_id;
// To be filled out by the arch-specific code of the kernel before the driver interface is called.
extern driver_list Drv_LoadedDrivers;
extern driver_list Drv_LoadedFsDrivers;
extern symbol_table OBOS_KernelSymbolTable;

typedef struct driver_init_status
{
    obos_status status;
    // An additional message to print describing the failure. can be nullptr
    const char* context;
    // If true, the driver will be unloaded. Ignored if obos_is_success(status) == true.
    bool fatal;
} driver_init_status;
// id->main must be the current thread, otherwise this function silently fails.
OBOS_EXPORT void Drv_ExitDriver(struct driver_id* id, const driver_init_status* status);
