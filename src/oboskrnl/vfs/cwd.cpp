/*
	oboskrnl/vfs/cwd.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <memmanip.h>
#include <todo.h>

#include <vfs/cwd.h>
#include <vfs/index_node.h>
#include <vfs/fsnode.h>

#include <scheduler/cpu_local.h>
#include <scheduler/thread.h>

namespace obos
{
	namespace vfs
	{
		index_node* g_cwd;
//#define cwd ((scheduler::GetCPUPtr())->currentThread->cwd)
#define cwd g_cwd
		TODO("Make the cwd per-process");
		size_t TokenizePath(const char* path, string_view* out)
		{
			if (!path)
				return SIZE_MAX;
			size_t size = strlen(path);
			size_t nTokens = 0;
			if (!size)
				return 0;
			const char* iter = path;
			while (iter != (path + size))
			{
				size_t tokenSize = strchr(iter, '/')+1-iter;
				if (*iter == '/')
					goto end;
				tokenSize -= (size_t)(iter[tokenSize - 1] == '/');
				nTokens++;
				if (out)
				{
					char* token = (char*)memcpy(new char[tokenSize + 1], iter, tokenSize);
					token[tokenSize] = 0;
					out[nTokens - 1] = token;
				}
				end:
				iter = strchr(iter, '/') + 1;
			}
			return nTokens;
		}
		index_node* LookForIndexNode(const char* path, index_node* root)
		{
			if (!path)
				return nullptr;
			if (!root)
				root = cwd;
			if (root->type != index_node_type::Directory)
				return nullptr;
			string_view* tokens = nullptr;
			size_t nTokens = TokenizePath(path, tokens);
			if (!nTokens)
				return nullptr;
			tokens = new string_view[nTokens];
			TokenizePath(path, tokens);
			if (tokens[0][0] == '/')
				root = g_root;
			index_node* currentNode = root;
			if ((int)(currentNode->flags & index_node_flags::IsMountPoint))
				currentNode = currentNode->data.mPoint->root.head;
			else
				currentNode = currentNode->children.head;
			size_t recursionIndex = 0;
			while (currentNode)
			{
				if (recursionIndex >= nTokens)
				{
					delete[] tokens;
					return nullptr; // The path hasn't been found.
				}
				if (strcmp(tokens[recursionIndex], "."))
				{
					recursionIndex++;
					continue;
				}
				if (strcmp(tokens[recursionIndex], ".."))
				{
					recursionIndex++;
					currentNode = currentNode->parent;
					continue;
				}
				if (!strcmp(currentNode->entryName, tokens[recursionIndex]))
				{
					currentNode = currentNode->next;
					continue;
				}
				if (recursionIndex != (nTokens - 1) && currentNode->type != index_node_type::Directory)
				{
					delete[] tokens;
					return nullptr; // One of the path components isn't a directory, abort.
				}
				if (recursionIndex++ == (nTokens - 1))
				{
					delete[] tokens;
					return currentNode;
				}
				if ((int)(currentNode->flags & index_node_flags::IsMountPoint))
					currentNode = currentNode->data.mPoint->root.head;
				else
					currentNode = currentNode->children.head;
			}
			delete[] tokens;
			return nullptr;
		}
		bool ChangeWorkingDirectory(index_node* to)
		{
			if (!to)
				return false;
			// TODO: Check if 'to' is actually an index node.
			cwd = to;
			return true;
		}
		bool ChangeWorkingDirectory(const char* to)
		{
			index_node* newCWD = LookForIndexNode(to, cwd);
			cwd = newCWD;
			return true;
		}
	}
}