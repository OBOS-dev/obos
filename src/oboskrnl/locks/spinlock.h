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
		// This lock raises the IRQL to MASK_ALL (0xf) when it's locked.
		class SpinLock
		{
		public:
			SpinLock() = default;

			bool Lock();
			bool Unlock();
			bool Locked() const;
		private:
			bool m_locked = false;
			uint8_t m_oldIRQL = 0;
		};
	}
}