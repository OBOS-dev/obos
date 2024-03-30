/*
    oboskrnl/arch/x86_64/mm/vmm_context.cpp

    Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <todo.h>

#include <arch/x86_64/mm/vmm_context.h>
#include <arch/x86_64/mm/palloc.h>

namespace obos
{
    namespace arch
    {
        bool pg_context::alloc()
        {
            m_context = new internal_context{};
            m_context->cr3 = (PageMap*)AllocatePhysicalPages(1, false);
            m_context->references = 1;
            return false;
        }
        bool pg_context::free()
        {
            if (!m_context)
                return false;
            if (!(--m_context->references))
            { 
                FreePhysicalPages((uintptr_t)m_context->cr3, 1);
                delete m_context;
            }
            m_context = nullptr;
            return true;
        }
        pg_context::~pg_context()
        {
            free();
        }
    }
}