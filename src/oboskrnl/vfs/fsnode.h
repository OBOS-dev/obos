/*
	oboskrnl/vfs/fsnode.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/file_attributes.h>
#include <vfs/index_node.h>

#include <locks/spinlock.h>

namespace obos
{
	namespace vfs
	{
		struct fsnode
		{
			void* data = nullptr;
			size_t len = 0;
			file_attribs attribs = file_attribs::Default;
			size_t references;
			locks::SpinLock lock;
		};
		struct mpoint
		{
			struct block_device* dev;
			index_node_list root;
			index_node* representative;
			size_t references;
			// only locked when we are being modified.
			locks::SpinLock lock;
		};
	}
}