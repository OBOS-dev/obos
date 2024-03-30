/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

namespace obos
{
	namespace locks
	{
		// Only to be used in kernel-mode.
		class SpinLock
		{
		public:
			SpinLock() = default;
			SpinLock(uint8_t minimumIRQL)
				: m_minimumIRQL { minimumIRQL }
			{}

			bool Lock();
			bool Unlock();
			bool Locked() const;
		private:
			bool m_locked = false;
			uint8_t m_oldIRQL = 0, m_minimumIRQL = 0x2 /* The scheduler can't run at this IRQL or higher. */;
		};
	}
}