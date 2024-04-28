/*
	oboskrnl/vfs/file_perms.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/vfs_string.h>

namespace obos
{
	namespace vfs
	{
		enum class file_perm_flags
		{
			Default = 0,
			Readable = 0x1,
			Writeable = 0x2,
			Executable = 0x4,
			Mask = Readable | Writeable | Executable,
		};
		inline file_perm_flags operator|(file_perm_flags right, file_perm_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_perm_flags::Mask;
			b &= (uint32_t)file_perm_flags::Mask;
			return (file_perm_flags)(a | b);
		}
		inline file_perm_flags operator&(file_perm_flags right, file_perm_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_perm_flags::Mask;
			b &= (uint32_t)file_perm_flags::Mask;
			return (file_perm_flags)(a & b);
		}
		inline file_perm_flags operator^(file_perm_flags right, file_perm_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_perm_flags::Mask;
			b &= (uint32_t)file_perm_flags::Mask;
			return (file_perm_flags)(a ^ b);
		}
		inline file_perm_flags operator~(file_perm_flags f)
		{
			uint32_t a = (uint32_t)f;
			a &= (uint32_t)file_perm_flags::Mask;
			return (file_perm_flags)~a;
		}
		struct basic_perm_info
		{
			string_view name;
			file_perm_flags flags;
		};
		struct basic_perm_info_node
		{
			basic_perm_info* data;
			basic_perm_info_node *next, *prev;
		};
		struct perm_info
		{
			// NOTE: A hash map would be a lot more suitable in this case.
			// TODO: Make a hash map structure
			basic_perm_info_node *head, *tail;
			size_t nNodes;
		};
	}
}