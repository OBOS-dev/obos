# dependencies/flanterm.cmake

# Copyright (c) 2025 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(flanterm
		GIT_REPOSITORY https://codeberg.org/mintsuki/flanterm.git
		GIT_TAG v2.0.0
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/flanterm
	)
	FetchContent_MakeAvailable(flanterm)
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_private.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
	file(MAKE_DIRECTORY "${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_backends")
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_backends/fb.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/flanterm_backends/")
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_backends/fb_private.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/flanterm_backends/")
endif()
list (APPEND oboskrnl_sources 
	"${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm.c"
	"${CMAKE_SOURCE_DIR}/dependencies/flanterm/src/flanterm_backends/fb.c"
)
