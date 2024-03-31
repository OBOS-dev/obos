# dependencies/uacpi.cmake

# Copyright (c) 2024 Omar Berrow

include(FetchContent)

FetchContent_Declare(uACPI
	GIT_REPOSITORY https://github.com/UltraOS/uACPI.git
	SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uACPI
)
FetchContent_MakeAvailable(uACPI)
file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uACPI/include/uacpi" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
set (uacpi_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uACPI/uacpi.cmake CACHE INTERNAL "The uACPI CMake file.")
