set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_SYSTEM_PROCESSOR "m68k")

find_program(HAS_CROSS_COMPILER "m68k-obos-g++")
if (NOT HAS_CROSS_COMPILER)
	message(FATAL_ERROR "No m68k-obos cross compiler in the PATH!")
endif()

if (OBOS_USE_CLANG)
	message(WARNING "OBOS does not support m68k clang")
endif()

set(CMAKE_C_COMPILER "m68k-obos-gcc")
set(CMAKE_CXX_COMPILER "m68k-obos-g++")
set(CMAKE_ASM-ATT_COMPILER ${CMAKE_C_COMPILER})
set(CMAKE_C_COMPILER_WORKS true)
set(CMAKE_CXX_COMPILER_WORKS true)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

execute_process(COMMAND m68k-obos-gcc -print-file-name=libgcc.a OUTPUT_VARIABLE LIBGCC_TARGET)

string(STRIP "${LIBGCC_TARGET}" LIBGCC_TARGET)

find_program(HAS_m68k_elf_objcopy "m68k-obos-objcopy")
find_program(HAS_m68k_elf_nm "m68k-obos-nm")
if (HAS_m68k_objcopy)
	set(OBJCOPY "m68k-obos-objcopy")
else()
	set(OBJCOPY "objcopy")
endif()
if (HAS_x86_64_elf_nm)
	set(NM "m68k-obos-nm")
else()
	set(NM "nm")
endif()
set(TARGET_COMPILE_OPTIONS_C -fno-omit-frame-pointer -msoft-float)
set(TARGET_DRIVER_COMPILE_OPTIONS_C -fno-omit-frame-pointer -msoft-float)
set(TARGET_LINKER_OPTIONS -mcpu=68040 -z max-page-size=4096)
set(TARGET_DRIVER_LINKER_OPTIONS -mcpu=68040 -z max-page-size=4096)

if (OBOS_ENABLE_KASAN)
	add_compile_options($<$<COMPILE_LANGUAGE:C>:-fasan-shadow-offset=0>)
endif()

add_compile_options("-mcpu=68040")

list (APPEND oboskrnl_sources 
	"arch/m68k/entry.c" "arch/m68k/asm_helpers.S" "arch/m68k/irql.c" "arch/m68k/driver_loader.c"
	"arch/m68k/irq.c" "arch/m68k/isr.S" "arch/m68k/thread_ctx.S" "arch/m68k/thread_ctx.c"
	"arch/m68k/mmu.c" "arch/m68k/pmm.c" "arch/m68k/exception_handlers.c" "arch/m68k/goldfish_rtc.c"
	"arch/m68k/goldfish_pic.c"
)

add_compile_definitions(
	OBOS_PAGE_SIZE=4096 
	# OBOS_HUGE_PAGE_SIZE=8192
	OBOS_HUGE_PAGE_SIZE=4096 
	OBOS_KERNEL_ADDRESS_SPACE_BASE=0xc0000000 OBOS_KERNEL_ADDRESS_SPACE_LIMIT=0xfffff000
	OBOS_USER_ADDRESS_SPACE_BASE=0x1000 OBOS_USER_ADDRESS_SPACE_LIMIT=0x80000000
	OBOS_TIMER_IS_DEADLINE=
	OBOS_ARCH_USES_SOFT_FLOAT=1
	OBOS_ARCH_EMULATED_IRQL=1
	OBOS_ARCH_HAS_USR_MEMCPY=0
    OBOS_ARCH_HAS_MEMSET=0
    OBOS_ARCH_HAS_MEMZERO=0
    OBOS_ARCH_HAS_MEMCPY=0
    OBOS_ARCH_HAS_MEMCMP=0
    OBOS_ARCH_HAS_MEMCMP_B=0
    OBOS_ARCH_HAS_STRCMP=0
    OBOS_ARCH_HAS_STRNCMP=0
    OBOS_ARCH_HAS_STRLEN=0
    OBOS_ARCH_HAS_STRNLEN=0
    OBOS_ARCH_HAS_STRCHR=0
    OBOS_ARCH_HAS_STRNCHR=0
)
add_compile_options($<$<CONFIG:Debug>:-g>)

set (OBOS_ARCHITECTURE "m68k")
set (OBOS_ARCHITECTURE_BITS 32) # 32-bit architecture.
set (OBOS_ARCHITECTURE_ENDIANNESS "Big-Endian") # Big-Endian architecture.
set (LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/build/m68k/link.ld")
set (DRIVER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/build/m68k/driver_link.ld")
set (OBOS_ARCHITECTURE_HAS_ACPI 0)
set (OBOS_ARCHITECTURE_HAS_PCI 0)
set (OBOS_IRQL_COUNT 8)
set (OBOS_UP 1) # The m68k doesn't support SMP, so we disable it as a [POSSIBLE] optimization
set (OBOS_ENABLE_UHDA 0)