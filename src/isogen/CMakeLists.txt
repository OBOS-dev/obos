# isogen/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

set (ISODIR ${CMAKE_SOURCE_DIR}/out/isodir/)

if (OBOS_ARCHITECTURE STREQUAL "m68k")
	return()
endif()

if (NOT EXISTS ${ISODIR})
	file (MAKE_DIRECTORY ${ISODIR})
endif()
if (NOT EXISTS ${ISODIR}/obos)
	file (MAKE_DIRECTORY ${ISODIR}/obos)
endif()
if (NOT EXISTS ${CMAKE_SOURCE_DIR}/config/hyper.cfg)
	message(FATAL_ERROR "No hyper configuration file detected!")
endif()

if (CMAKE_HOST_WIN32)
	set(SUPPRESS_OUTPUT > NUL 2>&1)
elseif(CMAKE_HOST_LINUX)
	set(SUPPRESS_OUTPUT > /dev/null 2>&1)
endif()

add_custom_target(isogen ALL
	COMMAND ${OBJCOPY} -g ${OUTPUT_DIR}/oboskrnl ${ISODIR}/obos/oboskrnl ${SUPPRESS_OUTPUT}
	COMMAND	cmake -E copy ${CMAKE_SOURCE_DIR}/config/hyper.cfg ${ISODIR}/ ${SUPPRESS_OUTPUT}
	COMMAND	cmake -E copy ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_iso_boot ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND	cmake -E copy ${CMAKE_SOURCE_DIR}/config/hyper_uefi_boot.bin ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND	cmake -E copy ${CMAKE_SOURCE_DIR}/out/initrd ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND	cmake -E copy ${CMAKE_SOURCE_DIR}/config/initrd.tar ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND	xorriso -as mkisofs -b hyper_iso_boot -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot hyper_uefi_boot.bin -efi-boot-part --efi-boot-image --protective-msdos-label out/isodir -o ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT}
	COMMAND chmod +x ${hyper_install}
	COMMAND	${hyper_install} ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT}
	SOURCES ${CMAKE_SOURCE_DIR}/config/hyper.cfg ${CMAKE_SOURCE_DIR}/config/hyper_uefi_boot.bin ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_iso_boot ${CMAKE_SOURCE_DIR}/config/initrd.tar
	BYPRODUCTS ${ISODIR}/hyper.cfg
	BYPRODUCTS ${ISODIR}/EFI/BOOT/BOOTX64.efi
	BYPRODUCTS ${ISODIR}/hyper_iso_boot
	BYPRODUCTS ${ISODIR}/obos/oboskrnl
	BYPRODUCTS ${ISODIR}/obos/initrd
	BYPRODUCTS ${ISODIR}/obos/initrd.tar
	BYPRODUCTS ${OUTPUT_DIR}/obos.iso
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	COMMENT "Generating ${OUTPUT_DIR}/obos.iso"
)
add_dependencies(isogen oboskrnl)
if (EXISTS initrd)
	add_dependencies(isogen initrd)
endif()
