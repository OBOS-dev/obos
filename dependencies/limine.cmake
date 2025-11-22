# dependencies/limine.cmake

# Copyright (c) 2025 Omar Berrow

include(FetchContent)

FetchContent_Declare(Limine
	GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
	GIT_TAG v10.3.0-binary
	SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/limine
)
FetchContent_MakeAvailable(Limine)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
execute_process(
    COMMAND "make"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/limine
    )
    set (limine_exe ${CMAKE_SOURCE_DIR}/dependencies/limine/limine CACHE INTERNAL "x")
# elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "MSYS")
#     set (limine_exe ${CMAKE_SOURCE_DIR}/dependencies/limine/limine.exe CACHE INTERNAL "x")
else()
	message(FATAL_ERROR "You must be on to compile OBOS with Limine.")
endif()
file (COPY_FILE "${CMAKE_SOURCE_DIR}/dependencies/limine/limine.h" "${CMAKE_SOURCE_DIR}/dependencies/include/limine.h")