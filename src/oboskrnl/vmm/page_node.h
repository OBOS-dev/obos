/*
	oboskrnl/vmm/page_node.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vmm/pg_context.h>
#include <vmm/page_descriptor.h>

namespace obos
{
	namespace vmm
	{
		struct page_node
		{
			page_node *next, *prev;
			page_descriptor pd;
			Context* ctx;
		};
	}
}