# dependencies/limine.cmake

# Copyright (c) 2024 Omar Berrow

include(FetchContent)

file(MAKE_DIRECTORY "dependencies/include/limine")

if (CMAKE_HOST_WIN32)
	set (limine_install ${CMAKE_SOURCE_DIR}/dependencies/limine/limine.exe CACHE INTERNAL "The limine install binary")
elseif(CMAKE_HOST_LINUX)
	set (limine_install ${CMAKE_SOURCE_DIR}/dependencies/limine/limine CACHE INTERNAL "The limine install binary")
else()
	message(FATAL_ERROR "You must be on windows or linux to compile OBOS.")
endif()
FetchContent_Declare(Limine
	GIT_REPOSITORY https://github.com/limine-bootloader/limine.git
	GIT_TAG 61558c6da9c2b0d23bfb7bc74135345acded0b73
	SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/limine
)
FetchContent_MakeAvailable(Limine)
file (COPY_FILE "dependencies/limine/limine.h" "dependencies/include/limine/limine.h")