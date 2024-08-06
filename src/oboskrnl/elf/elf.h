#if OBOS_ARCHITECTURE_BITS == 64
#   include <elf/elf64.h>
#elif OBOS_ARCHITECTURE_BITS == 32
#   include <elf/elf32.h>
#else 
#   error Unsupported OBOS_ARCHITECTURE_BITS value for elf.
#endif