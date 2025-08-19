# dependencies/freebsd-e1000.cmake

# Copyright (c) 2025 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(freebsd-e1000
		GIT_REPOSITORY https://github.com/managarm/freebsd-e1000.git
		GIT_TAG import
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/freebsd-e1000
	)
	FetchContent_MakeAvailable(freebsd-e1000)
	
	file (GLOB E1000_HEADERS SRC_FILES "${CMAKE_SOURCE_DIR}/dependencies/freebsd-e1000/*.h")
	file (COPY ${E1000_HEADERS} DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/e1000/")
endif()
	
file (GLOB E1000_SOURCES SRC_FILES "${CMAKE_SOURCE_DIR}/dependencies/freebsd-e1000/*.c")
