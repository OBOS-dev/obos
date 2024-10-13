#pragma once

#include <int.h>

#ifdef __x86_64__
typedef uintptr_t page_table;
#elif __m68k__
typedef uintptr_t page_table;
#else
#   error Unknown architecture
#endif
