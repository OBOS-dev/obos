/*
	oboskrnl/vmm/pg_context.cpp

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <todo.h>
#include <klog.h>

#include <arch/vmm_context.h>
#include <vmm/pg_context.h>

namespace obos
{
	namespace vmm
	{
		TODO("Implement default constructor for obos::vmm::Context")
		Context::Context() noexcept
		{
			OBOS_ASSERTP(!true, "obos::vmm::Context::Context() is un-implemented.");
		}
		Context::~Context() noexcept
		{
			if (m_owns)
				m_internalContext->free();
			m_internalContext = nullptr;
			m_owns = false;
		}
	}
}