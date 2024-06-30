/*
 * oboskrnl/mm/page_node.h
 *
 * Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <error.h>

#include <mm/prot.h>

#include <utils/tree.h>

#include <locks/spinlock.h>

typedef struct page_node
{
	bool present : 1;
	bool huge_page : 1;
	bool dirty : 1;
	bool accessed : 1;
	bool pagedOut : 1;
	uint8_t uses : 8; // The age
	spinlock lock;
	uintptr_t addr;
	prot_flags protection;
	RB_ENTRY(page_node) rb_tree_node;
	struct
	{
		struct page_node* next;
		struct page_node* prev;
	} linked_list_node;
	struct context* owner;
} page_node;
page_node* MmH_AllocatePageNode(obos_status* status);
void MmH_RegisterUse(page_node* pg);
uint8_t MmH_LogicalSumOfUses(uint8_t uses);