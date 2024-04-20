/*
	oboskrnl/vfs/index_node.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/vfs_string.h>
#include <vfs/file_perms.h>

#include <locks/spinlock.h>

namespace obos
{
	namespace vfs
	{
		struct index_node_list
		{
			struct index_node *head, *tail;
			size_t nNodes;
			locks::SpinLock lock;
		};
		enum class index_node_type
		{
			Invalid,
			File,
			Directory,
		};
		enum class index_node_flags : uint32_t
		{
			Default = 0,
			IsMountPoint = 0x1,
			Mask = 0x1,
		};
		inline index_node_flags operator|(index_node_flags right, index_node_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)index_node_flags::Mask;
			b &= (uint32_t)index_node_flags::Mask;
			return (index_node_flags)(a | b);
		}
		inline index_node_flags operator&(index_node_flags right, index_node_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)index_node_flags::Mask;
			b &= (uint32_t)index_node_flags::Mask;
			return (index_node_flags)(a & b);
		}
		inline index_node_flags operator^(index_node_flags right, index_node_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)index_node_flags::Mask;
			b &= (uint32_t)index_node_flags::Mask;
			return (index_node_flags)(a ^ b);
		}
		inline index_node_flags operator~(index_node_flags f)
		{
			uint32_t a = (uint32_t)f;
			a &= (uint32_t)index_node_flags::Mask;
			return (index_node_flags)~a;
		}
		struct index_node
		{
			string_view filepath{};
			union
			{
				struct fsnode* fsNode;
				struct mpoint* mPoint;
			} data{};
			index_node_type type = index_node_type::Invalid;
			index_node_flags flags = index_node_flags::Default;
			locks::SpinLock lock{};
			perm_info permissionInfo{};

			index_node_list children{};
			index_node *next = nullptr, *prev = nullptr;
		};
	}
}