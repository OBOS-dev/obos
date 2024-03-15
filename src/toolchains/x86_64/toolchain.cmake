set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_SYSTEM_PROCESSOR "x86-64")

find_program(HAS_CROSS_COMPILER "x86_64-elf-g++")
if (NOT HAS_CROSS_COMPILER)
	message(FATAL_ERROR "No x86_64-elf cross compiler in the PATH!")
endif()

set(CMAKE_C_COMPILER "x86_64-elf-gcc")
set(CMAKE_CXX_COMPILER "x86_64-elf-g++")
set(CMAKE_ASM-ATT_COMPILER "x86_64-elf-gcc")
set(CMAKE_ASM_NASM_COMPILER "nasm")
set(CMAKE_C_COMPILER_WORKS true)
set(CMAKE_CXX_COMPILER_WORKS true)
set(CMAKE_ASM-ATT_COMPILER_WORKS true)
set(CMAKE_ASM_NASM_COMPILER_WORKS true)

set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

set(CMAKE_ASM_NASM_OBJECT_FORMAT "elf64")

execute_process(COMMAND x86_64-elf-gcc --print-file-name=libgcc.a OUTPUT_VARIABLE LIBGCC)

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
set(TARGET_COMPILE_OPTIONS_CPP -mno-red-zone -fno-omit-frame-pointer -mgeneral-regs-only -mcmodel=kernel)
set(TARGET_COMPILE_OPTIONS_C ${TARGET_COMPILE_OPTIONS_CPP})
set(TARGET_LINKER_OPTIONS -mcmodel=kernel)

set (oboskrnl_sources ${oboskrnl_sources} 
	"arch/x86_64/entry.cpp" "arch/x86_64/bgdt.asm" "arch/x86_64/irq/idt.cpp" "arch/x86_64/memmanip.asm"
    "arch/x86_64/irq/isr.asm" "arch/x86_64/trace.cpp" "arch/x86_64/exception_handlers.cpp" "arch/x86_64/asm_helpers.asm"
    "arch/x86_64/sdt.cpp" "arch/x86_64/irq/madt.cpp" "arch/x86_64/irq/apic.cpp" "arch/x86_64/mm/palloc.cpp"
    "arch/x86_64/mm/map.cpp" "arch/x86_64/mm/pmap_l4.cpp" "arch/x86_64/mm/vmm_context.cpp" "arch/x86_64/sse.asm"
	"arch/x86_64/smp.cpp" "arch/x86_64/smp_trampoline.asm"
)

set (OBOS_ARCHITECTURE "x86_64")
set (LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/src/toolchains/x86_64/link.ld")