# dependencies/zydis.cmake

# Copyright (c) 2024 Omar Berrow

include(FetchContent)

if (NOT EXISTS ${CMAKE_SOURCE_DIR}/dependencies/ZyDis)
	file (MAKE_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/ZyDis)
endif()
FetchContent_Declare(
	ZyDis
	URL https://github.com/zyantific/zydis/releases/download/v4.1.0/zydis-amalgamated.tar.gz
	DOWNLOAD_DIR ${CMAKE_SOURCE_DIR}/dependencies/ZyDis
)
FetchContent_MakeAvailable(ZyDis)
execute_process(
	COMMAND ${CMAKE_COMMAND} -E tar -xzf ${CMAKE_SOURCE_DIR}/dependencies/ZyDis/zydis-amalgamated.tar.gz
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/dependencies/ZyDis/
)
file (COPY "${CMAKE_SOURCE_DIR}/dependencies/ZyDis/amalgamated-dist/Zydis.h" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
set (zydis_c ${CMAKE_SOURCE_DIR}/dependencies/ZyDis/amalgamated-dist/Zydis.c CACHE INTERNAL "The amalgamated ZyDis source file.")
file (REMOVE ${CMAKE_SOURCE_DIR}/dependencies/ZyDis/zydis-amalgamated.tar.gz)
