/*
	oboskrnl/locks/spinlock.cpp

	Copyright (c) 2024 Omar Berrow
*/

#include <int.h>

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
		bool SpinLock::Lock()
		{
			uint8_t oldIRQL = 0;
			RaiseIRQL(0xf, &oldIRQL);
			const bool excepted = false;
			while (__atomic_load_n(&m_locked, __ATOMIC_SEQ_CST));
			while (__atomic_compare_exchange_n(&m_locked, (bool*)&excepted, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
			LowerIRQL(oldIRQL);
			return m_locked;
		}
		bool SpinLock::Unlock()
		{
			__atomic_store_n(&m_locked, false, __ATOMIC_SEQ_CST);
			return Locked();
		}
		bool SpinLock::Locked() const
		{
			return __atomic_load_n(&m_locked, __ATOMIC_SEQ_CST);
		}
	}
}