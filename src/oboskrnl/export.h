#pragma once

#ifndef __INTELLISENSE__
#	define OBOS_WEAK __attribute__((weak))
#else
#	define OBOS_WEAK
#endif
#ifdef OBOS_DRIVER
#	ifndef __INTELLISENSE__
#		define OBOS_EXPORT __attribute__((weak))
#	else
#		define OBOS_EXPORT
#	endif
#elif defined(OBOS_KERNEL)
#	define OBOS_EXPORT
#else
#	define OBOS_EXPORT
#endif