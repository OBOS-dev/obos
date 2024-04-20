/*
	oboskrnl/vfs/file_attributes.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

namespace obos
{
	namespace vfs
	{
		enum class file_attribs
		{
			Default = 0,
			Read = 0x1,
			Write = 0x2,
			TemporaryFile = 0x4,
			Mask = Read|Write|TemporaryFile,
		};
		inline file_attribs operator|(file_attribs right, file_attribs left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_attribs::Mask;
			b &= (uint32_t)file_attribs::Mask;
			return (file_attribs)(a | b);
		}
		inline file_attribs operator&(file_attribs right, file_attribs left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_attribs::Mask;
			b &= (uint32_t)file_attribs::Mask;
			return (file_attribs)(a & b);
		}
		inline file_attribs operator^(file_attribs right, file_attribs left)
		{
			uint32_t a = (uint32_t)right;
			uint32_t b = (uint32_t)left;
			a &= (uint32_t)file_attribs::Mask;
			b &= (uint32_t)file_attribs::Mask;
			return (file_attribs)(a ^ b);
		}
		inline file_attribs operator~(file_attribs f)
		{
			uint32_t a = (uint32_t)f;
			a &= (uint32_t)file_attribs::Mask;
			return (file_attribs)~a;
		}
	}
}