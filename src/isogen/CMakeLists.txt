# isogen/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

# The only point of this subdirectory is to provide a target which builds the iso.

set (SOURCE_DIR ${CMAKE_SOURCE_DIR})
if (CMAKE_HOST_WIN32)
	set (COPY_FILE_COMMAND copy)
	set (PS "\\")
	string (REPLACE "/" "\\" SOURCE_DIR ${SOURCE_DIR})
elseif(CMAKE_HOST_UNIX)
	set (COPY_FILE_COMMAND cp)
	set (PS "/")
endif()

set (ISODIR ${SOURCE_DIR}${PS}isodir${PS})

if (NOT EXISTS ${ISODIR})
	file (MAKE_DIRECTORY ${ISODIR})
endif()
if (NOT EXISTS ${ISODIR}/obos)
	file (MAKE_DIRECTORY ${ISODIR}/obos)
endif()
if (NOT EXISTS ${CMAKE_SOURCE_DIR}/config/hyper.cfg)
	message(FATAL_ERROR "No hyper configuration file detected!")
endif()
add_custom_target(isogen ALL
	COMMAND ${OBJCOPY} -g ${OUTPUT_DIR}/oboskrnl ${ISODIR}/obos/oboskrnl
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}config${PS}hyper.cfg ${ISODIR}${PS}
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}dependencies${PS}hyper${PS}hyper_iso_boot ${ISODIR}
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}config${PS}hyper_uefi_boot.bin ${ISODIR}
	COMMAND	xorriso -as mkisofs -b hyper_iso_boot -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot hyper_uefi_boot.bin -efi-boot-part --efi-boot-image --protective-msdos-label isodir -o ${OUTPUT_DIR}/obos.iso
	COMMAND	${hyper_install} ${OUTPUT_DIR}${PS}obos.iso
	SOURCES ${CMAKE_SOURCE_DIR}/config/hyper.cfg ${CMAKE_SOURCE_DIR}/config/hyper_uefi_boot.bin ${CMAKE_SOURCE_DIR}/dependencies/hyper/hyper_iso_boot
	DEPENDS oboskrnl
	BYPRODUCTS ${ISODIR}/hyper.cfg
	BYPRODUCTS ${ISODIR}/EFI/BOOT/BOOTX64.efi
	BYPRODUCTS ${ISODIR}/hyper_iso_boot
	BYPRODUCTS ${ISODIR}/obos/oboskrnl
	BYPRODUCTS ${OUTPUT_DIR}/obos.iso
	# DEPENDS oboskrnl
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	COMMENT "Generating ${OUTPUT_DIR}/obos.iso"
)