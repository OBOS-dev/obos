# dependencies/uacpi.cmake

# Copyright (c) 2024-2025 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(uACPI
		#GIT_REPOSITORY https://github.com/UltraOS/uACPI.git
		GIT_REPOSITORY https://github.com/uACPI/uACPI.git
		GIT_TAG 1d636a34152dc82833c89175b702f2c0671f04e3
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uACPI
	)
	FetchContent_MakeAvailable(uACPI)
	
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uACPI/include/uacpi" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
endif()
set (uacpi_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uACPI/uacpi.cmake CACHE INTERNAL "The uACPI CMake file.")
