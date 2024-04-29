/*
	oboskrnl/vfs/whence.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vfs
	{
		enum class Whence
		{
			Set,
			Beginning = Set,
			Current,
			End,
		};
	}
}