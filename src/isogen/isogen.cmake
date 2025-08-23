# isogen/isogen.cmake

# Copyright (c) 2024-2025 Omar Berrow

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

add_custom_command(OUTPUT ${OUTPUT_DIR}/obos.iso
	COMMAND ${OBJCOPY} -g ${OUTPUT_DIR}/oboskrnl ${ISODIR}/obos/oboskrnl ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/config/hyper.cfg ${ISODIR}/ ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_iso_boot ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/config/hyper_uefi_boot.bin ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/out/initrd ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/config/initrd.tar ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND xorriso -as mkisofs -b hyper_iso_boot -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot hyper_uefi_boot.bin -efi-boot-part --efi-boot-image --protective-msdos-label out/isodir -o ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT}
	COMMAND chmod +x ${hyper_install}
	COMMAND ${hyper_install} ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT}
	BYPRODUCTS ${ISODIR}/hyper.cfg
			   ${ISODIR}/hyper_iso_boot
			   ${ISODIR}/hyper_uefi_boot.bin
			   ${ISODIR}/obos/oboskrnl
			   ${ISODIR}/obos/initrd
			   ${ISODIR}/obos/initrd.tar
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/config/initrd.tar ${CMAKE_SOURCE_DIR}/config/hyper.cfg oboskrnl initrd
)
add_custom_target (isogen ALL
	DEPENDS oboskrnl initrd ${OUTPUT_DIR}/obos.iso
)
