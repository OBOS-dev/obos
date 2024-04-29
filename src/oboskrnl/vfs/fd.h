/*
	oboskrnl/vfs/fd.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <vfs/off_t.h>
#include <vfs/whence.h>

namespace obos
{
	namespace vfs
	{
		enum class file_open_flags
		{
			Default = 0,
			/// <summary>
			/// When set, all Write()s fail on this file descriptor.
			/// </summary>
			ReadOnly = 1,
			/// <summary>
			/// When set, the file's data is cleared on Open().<br>
			/// Incompatible with ReadOnly.
			/// </summary>
			Trunc = 2,
			/// <summary>
			/// When set, the file position is moved to the end on Open().
			/// </summary>
			Append = 4,
			/// <summary>
			/// When set, the file is created if it doesn't exist already. When mixed with Trunc, the file is guaranteed to exist, and have no data.
			/// </summary>
			Create = 8,
			Mask = ReadOnly | Trunc | Append | Create,
		};
		class file_descriptor
		{
		public:
			file_descriptor() = default;
			
			bool Open(const char* path, file_open_flags flags);

			size_t Read(void* into, size_t count);
			size_t Write(const void* buf, size_t count);
			size_t Truncate(size_t to);

			size_t Filesize();

			bool Eof();

			void Seek(off_t where, Whence whence);

			file_descriptor* Duplicate();

			bool Close();
		private:
			struct index_node* m_indexNode;
			off_t m_currentOffset = 0;
		};
		inline file_open_flags operator|(file_open_flags right, file_open_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_open_flags::Mask;
			b &= (uint32_t)file_open_flags::Mask;
			return (file_open_flags)(a | b);
		}
		inline file_open_flags operator&(file_open_flags right, file_open_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_open_flags::Mask;
			b &= (uint32_t)file_open_flags::Mask;
			return (file_open_flags)(a & b);
		}
		inline file_open_flags operator^(file_open_flags right, file_open_flags left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_open_flags::Mask;
			b &= (uint32_t)file_open_flags::Mask;
			return (file_open_flags)(a ^ b);
		}
		inline file_open_flags operator~(file_open_flags f)
		{
			uint32_t a = (uint32_t)f;
			a &= (uint32_t)file_open_flags::Mask;
			return (file_open_flags)~a;
		}
	}
}