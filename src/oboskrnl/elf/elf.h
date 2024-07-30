#if OBOS_ARCHITECTURE_BITS == 64
#   include <elf/elf64.h>
#else
#   error Unsupported OBOS_ARCHITECTURE_BITS value for elf.
#endif