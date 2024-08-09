set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_SYSTEM_PROCESSOR "m68k")

find_program(HAS_CROSS_COMPILER "m68k-elf-g++")
if (NOT HAS_CROSS_COMPILER)
	message(FATAL_ERROR "No m68k-elf cross compiler in the PATH!")
endif()

set(CMAKE_C_COMPILER "m68k-elf-gcc")
set(CMAKE_CXX_COMPILER "m68k-elf-g++")
set(CMAKE_ASM-ATT_COMPILER ${CMAKE_C_COMPILER})
set(CMAKE_C_COMPILER_WORKS true)
set(CMAKE_CXX_COMPILER_WORKS true)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

execute_process(COMMAND m68k-elf-gcc -print-file-name=libgcc.a OUTPUT_VARIABLE LIBGCC)

string(STRIP "${LIBGCC}" LIBGCC)

find_program(HAS_m68k_elf_objcopy "m68k-elf-objcopy")
find_program(HAS_m68k_elf_nm "m68k-elf-nm")
if (HAS_m68k_objcopy)
	set(OBJCOPY "m68k-elf-objcopy")
else()
	set(OBJCOPY "objcopy")
endif()
if (HAS_x86_64_elf_nm)
	set(NM "m68k-elf-nm")
else()
	set(NM "nm")
endif()
set(TARGET_COMPILE_OPTIONS_C -fno-omit-frame-pointer -msoft-float)
set(TARGET_DRIVER_COMPILE_OPTIONS_C -fno-omit-frame-pointer -msoft-float)
set(TARGET_LINKER_OPTIONS -march=68040)
set(TARGET_DRIVER_LINKER_OPTIONS -march=68040)

if (DEFINED OBOS_ENABLE_KASAN)
	add_compile_options($<$<COMPILE_LANGUAGE:C>:-fasan-shadow-offset=0>)
endif()

add_compile_options("-march=68040")

list (APPEND oboskrnl_sources 
	"arch/m68k/entry.c" "arch/m68k/memmanip.c" "arch/m68k/asm_helpers.S" "arch/m68k/irql.c"
	"arch/m68k/irq.c" "arch/m68k/isr.S" "arch/m68k/thread_ctx.S" "arch/m68k/thread_ctx.c"
	"arch/m68k/mmu.c" "arch/m68k/pmm.c" "arch/m68k/exception_handlers.c" "arch/m68k/initial_swap.c"
	"arch/m68k/goldfish_pic.c" "arch/m68k/goldfish_rtc.c"
)

add_compile_definitions(
	OBOS_PAGE_SIZE=4096 
	# OBOS_HUGE_PAGE_SIZE=8192
	OBOS_HUGE_PAGE_SIZE=4096 
	OBOS_KERNEL_ADDRESS_SPACE_BASE=0xc0000000 OBOS_KERNEL_ADDRESS_SPACE_LIMIT=0xfffff000
	OBOS_USER_ADDRESS_SPACE_BASE=0x1000 OBOS_USER_ADDRESS_SPACE_LIMIT=0x80000000
	OBOS_TIMER_IS_DEADLINE=
	OBOS_ARCH_USES_SOFT_FLOAT=1
)
add_compile_options($<$<CONFIG:Debug>:-g>)

set (OBOS_ARCHITECTURE "m68k")
set (OBOS_ARCHITECTURE_BITS 32) # 32-bit architecture.
set (OBOS_ARCHITECTURE_ENDIANNESS "Big-Endian") # Little-Endian architecture.
set (LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/build/m68k/link.ld")
set (DRIVER_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/build/m68k/driver_link.ld")
set (OBOS_ARCHITECTURE_HAS_ACPI 0)
set (OBOS_IRQL_COUNT 8)
set (OBOS_UP 1) # The m68k doesn't support SMP, so we disable it as a [POSSIBLE] optimization