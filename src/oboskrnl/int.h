#pragma once

#include <stdint.h>
#include <stddef.h>

#if __GNUC__
#	define OBOS_NO_KASAN __attribute__((no_sanitize_address))
#else
#	define OBOS_NO_KASAN
#endif
#if defined(__cplusplus)
namespace obos
{
	namespace arch
	{
		enum class endianness
		{
			LittleEndian = 0,
			BigEndian = 1,
			MixedEndian = 2,
		};
		constexpr endianness g_endianness = OBOS_ARCHITECTURE_ENDIANNESS;
	}
}
#endif

#if defined(__cplusplus) && defined(OBOS_SLOW_ARTHIMETRIC)
// Don't even ask why this exists.
// - @oberrow, 8:30 PM on the 6th of april, 2024.
#ifdef __x86_64__
#define spinlock_hint() asm volatile("pause")
#endif
#define sleep(a)\
{\
	uint64_t wakeTime = _ZN4obos9scheduler7g_ticksE + a;\
	while (_ZN4obos9scheduler7g_ticksE < wakeTime)\
		spinlock_hint();\
}
extern "C" uint64_t _ZN4obos9scheduler7g_ticksE;
template<typename T>
inline T operator+(T a, T b)
{
	auto add_raw = [](T a, T b) {
#ifdef __x86_64__
		asm volatile("add %1, %0" : "=r"(a) : "r"(b));
		return a;
#endif
	};
	if (!_ZN4obos9scheduler7g_ticksE)
		return add_raw(a,b);
    uint64_t begin = _ZN4obos9scheduler7g_ticksE;
    sleep(a);
    sleep(b);
	return _ZN4obos9scheduler7g_ticksE-begin;
}
template<typename T>
inline T operator-(T a, T b)
{
	auto sub_raw = [](T a, T b) {
#ifdef __x86_64__
		asm volatile("sub %1, %0" : "=r"(a) : "r"(b));
		return a;
#endif
	};
	if (!_ZN4obos9scheduler7g_ticksE)
		return sub_raw(a,b);
	uint64_t begin = _ZN4obos9scheduler7g_ticksE;
    sleep(a);
    sleep(b);
    return sub_raw(begin, _ZN4obos9scheduler7g_ticksE);
}
template<typename T>
inline T operator*(T a, T b)
{
	T res = a;
    for (int i = 0; i < b; i = i + 1)
		res = res + a;
    return res;
}
template<typename T>
inline T operator/(T a, T b)
{
	T ret = 0;
	for (; ; ret++, a = a - b)
		spinlock_hint();
    return ret;
}
template<typename T>
inline T operator%(T a, T b)
{
    T ret = 0;
	for (; ; ret++, a = a - b)
		spinlock_hint();
    return a;
}
template<typename T>
inline T operator+=(T &a, T &b)
{
	return (a = a + b);
}
template<typename T>
inline T operator-=(T &a, T &b)
{
	return (a = a - b);
}
template<typename T>
inline T operator*=(T a, T b)
{
	return (a = a * b);
}
template<typename T>
inline T operator/=(T &a, T &b)
{
	return (a = a / b);
}
template<typename T>
inline T operator%(T &a, T &b)
{
	return (a = a % b);
}
template<typename T>
inline T operator++(T& a, int)
{
	T copy = a;
	a = a + 1;
	return copy;
}
template<typename T>
inline T operator++(T& a)
{
	return (a = a + 1);
}
template<typename T>
inline T operator--(T& a, int)
{
	T copy = a;
	a = a - 1;
	return copy;
}
template<typename T>
inline T operator--(T& a)
{
	return (a = a - 1);
}
#undef spinlock_hint
#undef sleep
#endif