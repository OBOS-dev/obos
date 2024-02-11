# dependencies/hyper.cmake

# Copyright (c) 2024 Omar Berrow

include(FetchContent)

file(MAKE_DIRECTORY "dependencies/include/UltraProtocol")
file(DOWNLOAD
	https://github.com/UltraOS/Hyper/releases/download/v0.7.0/hyper_iso_boot
	${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_iso_boot
)
file(DOWNLOAD
	https://github.com/UltraOS/Hyper/releases/download/v0.7.0/BOOTX64.EFI
	${CMAKE_SOURCE_DIR}/dependencies/hyper/BOOTX64.efi
)
if (CMAKE_HOST_WIN32)
	set (hyper_install ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_install-win64.exe)
elseif(CMAKE_HOST_LINUX)
	set (hyper_install ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_install-linux-x86_64)
else()
	message(FATAL_ERROR "You must be on windows or linux to compile OBOS.")
endif()
file(DOWNLOAD
	https://github.com/UltraOS/Hyper/releases/download/v0.7.0/hyper_install-win64.exe
	${hyper_install}
)
FetchContent_Declare(UltraProtocol
	GIT_REPOSITORY https://github.com/UltraOS/UltraProtocol.git
	GIT_TAG 769ba9c7b3ea4907873ebd120ade5e3749b82528
	SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/UltraProtocol
)
FetchContent_MakeAvailable(UltraProtocol)
file (COPY_FILE "dependencies/UltraProtocol/ultra_protocol.h" "dependencies/include/UltraProtocol/ultra_protocol.h")