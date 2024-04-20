/*
	oboskrnl/vfs/vfs_string.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <memmanip.h>

namespace obos
{
	namespace vfs
	{
		struct string_view
		{
			string_view() = default;
			string_view(const char* string)
				:str{ string }, len{ strlen(string) }
			{}
			string_view(const char* string, size_t length)
				:str{ string }, len{ length }
			{}
			const char* str;
			size_t len;
			operator const char*() const { return str; }
		};
	}
}