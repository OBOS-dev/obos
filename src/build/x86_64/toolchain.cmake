set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_SYSTEM_PROCESSOR "x86-64")

find_program(HAS_CROSS_COMPILER "x86_64-elf-g++")
if (NOT HAS_CROSS_COMPILER)
	message(FATAL_ERROR "No x86_64-elf cross compiler in the PATH!")
endif()

set(CMAKE_C_COMPILER "x86_64-elf-gcc")
set(CMAKE_ASM-ATT_COMPILER "x86_64-elf-gcc")
set(CMAKE_ASM_NASM_COMPILER "nasm")
set(CMAKE_C_COMPILER_WORKS true)
set(CMAKE_ASM_NASM_COMPILER_WORKS true)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

set(CMAKE_ASM_NASM_OBJECT_FORMAT "elf64")

execute_process(COMMAND x86_64-elf-gcc -print-file-name=no-red-zone/libgcc.a OUTPUT_VARIABLE LIBGCC)
if (LIBGCC STREQUAL "no-red-zone/libgcc.a")
	# Use normal libgcc
	execute_process(COMMAND x86_64-elf-gcc -print-file-name=libgcc.a OUTPUT_VARIABLE LIBGCC)
endif()

string(STRIP "${LIBGCC}" LIBGCC)

find_program(HAS_x86_64_elf_objcopy "x86_64-elf-objcopy")
find_program(HAS_x86_64_elf_nm "x86_64-elf-nm")
if (HAS_x86_64_elf_objcopy)
	set(OBJCOPY "x86_64-elf-objcopy")
else()
	set(OBJCOPY "objcopy")
endif()
if (HAS_x86_64_elf_nm)
	set(NM "x86_64-elf-nm")
else()
	set(NM "nm")
endif()
set(TARGET_COMPILE_OPTIONS_C -mno-red-zone -fno-omit-frame-pointer -mgeneral-regs-only -mcmodel=kernel -march=x86-64-v2)
set(TARGET_DRIVER_COMPILE_OPTIONS_C -mno-red-zone -fno-omit-frame-pointer -mgeneral-regs-only -march=x86-64-v2)
set(TARGET_LINKER_OPTIONS -mcmodel=kernel)
set(TARGET_DRIVER_LINKER_OPTIONS)

list (APPEND oboskrnl_sources 
	"arch/x86_64/entry.c" "arch/x86_64/bgdt.asm" "arch/x86_64/idt.c" "arch/x86_64/isr.asm"
	"arch/x86_64/asm_helpers.asm"
)

add_compile_definitions(__x86_64__=1)

set (OBOS_ARCHITECTURE "x86_64")
set (OBOS_ARCHITECTURE_BITS 64) # 64-bit architecture.
set (OBOS_ARCHITECTURE_ENDIANNESS "Little-Endian") # Little-Endian architecture.
set (LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/toolchains/x86_64/link.ld")