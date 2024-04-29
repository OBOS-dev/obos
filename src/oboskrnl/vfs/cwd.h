/*
	oboskrnl/vfs/cwd.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vfs
	{
		bool ChangeWorkingDirectory(struct index_node* to);
		// to can be a relative path.
		bool ChangeWorkingDirectory(const char* to);
		// root is overridden to 'g_root' if path[0] == '/'
		// If root is nullptr, it is assumed to be the current process' CWD.
		// './' is ignored.
		index_node* LookForIndexNode(const char* path, index_node* root);
		// Returns the number of tokens, or SIZE_MAX on failure.
		// out can be nullptr.
		size_t TokenizePath(const char* path, struct string_view* out);
	}
}