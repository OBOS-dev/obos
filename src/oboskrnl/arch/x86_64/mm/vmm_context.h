/*
	oboskrnl/arch/x86_64/mm/vmm_context.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>

#include <arch/x86_64/mm/pmap_l4.h>

namespace obos
{
	namespace arch
	{
		struct internal_context
		{
			PageMap* cr3;
			size_t references;
		};
		class pg_context
		{
		public:
			pg_context() = default;

			void set(internal_context* ctx)
			{ m_context = ctx; }
			
			bool alloc();
			bool free();

			PageMap* getCR3() const { if (!m_context) return nullptr; return m_context->cr3; }

			~pg_context();
		private:
			internal_context* m_context = nullptr;
		};
	}
}