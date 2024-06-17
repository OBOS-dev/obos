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

typedef struct page_node
{
	bool present;
	uintptr_t addr;
	prot_flags protection;
	bool dirty;
	bool accessed;
	uint8_t uses;
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