# dependencies/limine.cmake

# Copyright (c) 2024 Omar Berrow


if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)
	file(MAKE_DIRECTORY "dependencies/include/limine")
	
	FetchContent_Declare(Limine
		GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
		GIT_TAG 61558c6da9c2b0d23bfb7bc74135345acded0b73
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/limine
	)
	FetchContent_MakeAvailable(Limine)
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/limine/limine.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/limine/")
endif()

if (CMAKE_HOST_WIN32)
	set (limine_install ${CMAKE_SOURCE_DIR}/dependencies/limine/limine.exe CACHE INTERNAL "The limine install binary")
elseif(CMAKE_HOST_UNIX)
	set (limine_install ${CMAKE_SOURCE_DIR}/dependencies/limine/limine CACHE INTERNAL "The limine install binary")
	if (OBOS_REFRESH_DEPENDENCIES)
		execute_process(
			COMMAND make
			WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/limine/
		)
	endif()
else()
	message(FATAL_ERROR "You must be on windows or linux to compile OBOS.")
endif()