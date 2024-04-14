/*
	oboskrnl/locks/spinlock.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>
#include <klog.h>

#include <irq/irql.h>

#include <locks/spinlock.h>

#ifdef __INTELLISENSE__
template<class type>
bool __atomic_compare_exchange_n(type* ptr, type* expected, type desired, bool weak, int success_memorder, int failure_memorder);
template<class type>
void __atomic_store_n(type* ptr, type val, int);
template<class type>
type __atomic_load_n(type* ptr, int);
#endif

namespace obos
{
	namespace locks
	{
#if defined(__i386__) || defined(__x86_64__)
#	define spinlock_delay() asm volatile("pause")
#else
#	error Unknown arch.
#endif
		bool SpinLock::Lock()
		{
			m_oldIRQL = 0xff;
			if (GetIRQL() < m_minimumIRQL)
				RaiseIRQL(m_minimumIRQL, &m_oldIRQL);
			const bool excepted = false;
			size_t spin = 0;
			bool shouldPrint = true;
			while (__atomic_load_n(&m_locked, __ATOMIC_SEQ_CST))
			{
#ifdef OBOS_DEBUG
				if (spin++ >= 100000000 && shouldPrint)
				{
					logger::debug("Spin lock is hanging. Possible recursive lock or forgotten unlock.\n");
					logger::stackTrace(nullptr);
					shouldPrint = false;
					spin = 0;
				}
#endif
				spinlock_delay();
			}
					shouldPrint = false;
			spin = 0;
			while (__atomic_compare_exchange_n(&m_locked, (bool*)&excepted, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			{
#ifdef OBOS_DEBUG
				if (spin++ >= 100000000 && shouldPrint)
				{
					logger::debug("Spin lock is hanging. Possible recursive lock or forgotten unlock.\n");
					logger::stackTrace(nullptr);
					shouldPrint = false;
					spin = 0;
				}
#endif
				spinlock_delay();
			}
			return m_locked;
		}
		bool SpinLock::Unlock()
		{
			if (!Locked())
				return false; // We shouldn't be lowering the IRQL to an undefined value.
			__atomic_store_n(&m_locked, false, __ATOMIC_SEQ_CST);
			if (m_oldIRQL != 0xff)
				LowerIRQL(m_oldIRQL);
			return Locked();
		}
		bool SpinLock::Locked() const
		{
			return __atomic_load_n(&m_locked, __ATOMIC_SEQ_CST);
		}
	}
}