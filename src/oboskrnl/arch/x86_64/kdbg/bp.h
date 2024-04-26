/*
	oboskrnl/arch/x86_64/kdbg/bp.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace kdbg
	{
		struct bp
		{
			bp() = default;
			bp(uintptr_t rip);
			uint32_t idx;
			void setStatus(bool to);
			uintptr_t rip = 0;
			size_t hitCount = 0;
			bool enabled = false;
			bool awaitingSmpRefresh = false; // Set for breakpoints that are initialized before SMP initialization.
			struct
			{
				const char* name = nullptr;
				uintptr_t base = 0;
			} funcInfo{};
			~bp();
		};
		// struct bp_node
		// {
			// bp_node *next, *prev;
			// bp *data;
		// };
		// struct bp_list
		// {
			// bp_node *head, *tail;
			// size_t nBreakpoints;
			// void append(bp* breakpoint);
			// void remove(uint32_t idx);
			// bool contains(size_t idx);
			// bp& operator[](size_t idx);
			// bp_node* getNode(size_t idx);
		// };
	}

}