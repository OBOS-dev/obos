/*
	oboskrnl/vfs/open.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

#include <vfs/off_t.h>
#include <vfs/whence.h>
#include <vfs/index_node.h>
#include <vfs/vfs_string.h>
#include <vfs/fd.h>

namespace obos
{
	namespace vfs
	{
		index_node* g_root;

		bool file_descriptor::Open(const char* path, file_open_flags flags)
		{
			if (!path)
				return false;
			if ((int)(flags & ~file_open_flags::Mask))
				return false;
			
		}

		file_descriptor* file_descriptor::Duplicate()
		{
			file_descriptor* newFd = new file_descriptor{};
			newFd->m_indexNode = m_indexNode;
			newFd->m_currentOffset = m_currentOffset;
			m_indexNode->references++;
		}

		bool file_descriptor::Close()
		{

		}
	}
}