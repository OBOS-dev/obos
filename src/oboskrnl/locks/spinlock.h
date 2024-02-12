/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

namespace obos
{
	namespace locks
	{
		class SpinLock
		{
		public:
			SpinLock() = default;

			bool Lock();
			bool Unlock();
			bool Locked() const;
		private:
			bool m_locked;
		};
	}
}