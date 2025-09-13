# oboskrnl/arch/m68k/loader/CMakeLists.txt
#
# Copyright (c) 2024 Omar Berrow

# CMake file for the m68k loader "borrowed" from northport

add_executable(m68k_bootloader)

target_sources(m68k_bootloader PRIVATE
    "Entry.S" "Main.cpp" "Loader.cpp" "Memory.cpp" 
    "Syslib.cpp" "Util.cpp")

add_dependencies(m68k_bootloader oboskrnl)

set_source_files_properties(
    "Loader.cpp"
    PROPERTIES
    OBJECT_DEPENDS "${OUTPUT_DIR}/oboskrnl"
)

target_compile_options(m68k_bootloader
    PRIVATE "-w"
    PRIVATE "-fstack-protector-strong" 
    PRIVATE "-ffreestanding"
    PRIVATE "-std=c++17"
    PRIVATE "-fno-rtti"
    PRIVATE "-fno-exceptions" 
    PRIVATE "-fno-unwind-tables" 
    PRIVATE "-fno-asynchronous-unwind-tables"
    PRIVATE "-g"
    PRIVATE "-m68040"
)
target_link_options(m68k_bootloader
    PRIVATE "-nostdlib"
    PRIVATE "-static"
    PRIVATE "-T" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Linker.lds"
)
target_compile_definitions(m68k_bootloader
    PRIVATE NPL_ENABLE_LOGGING=1
    PRIVATE OUTPUT_DIRECTORY="${OUTPUT_DIR}"
)

target_include_directories(m68k_bootloader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})