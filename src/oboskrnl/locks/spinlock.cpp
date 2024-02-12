/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

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
			const bool excepted = false;
			while (__atomic_load_n(&m_locked, __ATOMIC_SEQ_CST));
			while (__atomic_compare_exchange_n(&m_locked, (bool*)&excepted, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
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