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
            TODO("Implement obos::arch::pg_context::alloc().")
            return false;
        }
        bool pg_context::free()
        {
            if (!m_context)
                return false;
            if (!(--m_context->references))
            { 
                FreePhysicalPages((uintptr_t)m_context->cr3, 1);
                TODO("Uncomment \"delete m_context\" when operator delete is implemented.");
                // delete m_context;
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