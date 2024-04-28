/*
	oboskrnl/locks/spinlock.h

	Copyright (c) 2024 Omar Berrow
*/

#pragma once

#include <int.h>
#include <export.h>

namespace obos
{
	namespace locks
	{
		// Only to be used in kernel-mode.
		// This lock should be held for as little time as possible.
		class SpinLock
		{
		public:
			SpinLock() = default;
			SpinLock(uint8_t minimumIRQL)
				: m_minimumIRQL { minimumIRQL }
			{}

			OBOS_EXPORT bool Lock();
			OBOS_EXPORT bool Unlock();
			OBOS_EXPORT bool Locked() const;
		private:
			volatile bool m_locked = false;
			uint8_t m_oldIRQL = 0, m_minimumIRQL = 0x2 /* The scheduler can't run at this IRQL or higher. */;
		};
	}
}