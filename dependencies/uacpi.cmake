# dependencies/uacpi.cmake

# Copyright (c) 2024 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(uACPI
		GIT_REPOSITORY https://github.com/UltraOS/uACPI.git
        GIT_TAG eda7c1b24e7b320d72313063d60bee2b3ef88510
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uACPI
	)
	FetchContent_MakeAvailable(uACPI)
	
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uACPI/include/uacpi" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
endif()
set (uacpi_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uACPI/uacpi.cmake CACHE INTERNAL "The uACPI CMake file.")
