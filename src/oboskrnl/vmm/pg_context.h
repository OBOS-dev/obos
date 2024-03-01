/*
	oboskrnl/vmm/pg_context.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <todo.h>

#include <arch/vmm_context.h>

namespace obos
{
	namespace vmm
	{
		class Context
		{
		public:
			Context() noexcept;
			Context(arch::pg_context* ctx) noexcept : m_internalContext{ ctx }, m_owns{ false } {}

			arch::pg_context* GetContext() const { return m_internalContext; }

			~Context() noexcept;
		private:
			arch::pg_context *m_internalContext = nullptr;
			bool m_owns = true;
		};
	}
}