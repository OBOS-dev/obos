# dependencies/uacpi.cmake

# Copyright (c) 2024-2025 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(uACPI
		# In honor of the old uACPI link
		#GIT_REPOSITORY https://github.com/UltraOS/uACPI.git
		GIT_REPOSITORY https://github.com/uACPI/uACPI.git
		GIT_TAG 2.1.0
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uACPI
	)
	FetchContent_MakeAvailable(uACPI)
	
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uACPI/include/uacpi" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
endif()
set (uacpi_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uACPI/uacpi.cmake CACHE INTERNAL "The uACPI CMake file.")
