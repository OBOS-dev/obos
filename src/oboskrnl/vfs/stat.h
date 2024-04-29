/*
	oboskrnl/vfs/stat.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/file_attributes.h>
#include <vfs/file_perms.h>
#include <vfs/index_node.h>

namespace obos
{
	namespace vfs
	{
		struct stat_t
		{
			file_attribs attrib;
			perm_info perm;
			size_t size;
			index_node_type type;
			const char* parent;
		};
		bool Stat(const char* filepath, stat_t* out);
	}
}