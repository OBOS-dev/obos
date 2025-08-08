# dependencies/uhda.cmake

# Copyright (c) 2025 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(uHDA
		GIT_REPOSITORY https://github.com/uDrivers/uHDA.git
		GIT_TAG main
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uHDA
	)
	FetchContent_MakeAvailable(uHDA)
	
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uHDA/include/uhda" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
endif()
set (uhda_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uHDA/uhda.cmake CACHE INTERNAL "The uHDA CMake file.")
