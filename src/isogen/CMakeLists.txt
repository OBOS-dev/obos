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
if (NOT EXISTS ${ISODIR}/limine)
	file (MAKE_DIRECTORY ${ISODIR}/limine)
endif()
if (NOT EXISTS ${CMAKE_SOURCE_DIR}/config/limine.cfg)
	message(FATAL_ERROR "No hyper configuration file detected!")
endif()
add_custom_target(isogen ALL
	COMMAND ${OBJCOPY} -g ${OUTPUT_DIR}/oboskrnl ${ISODIR}/obos/oboskrnl
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}config${PS}limine.cfg ${ISODIR}${PS}
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}dependencies${PS}limine${PS}limine-uefi-cd.bin ${ISODIR}${PS}limine
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}dependencies${PS}limine${PS}limine-bios-cd.bin ${ISODIR}${PS}limine
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}dependencies${PS}limine${PS}limine-bios-pxe.bin ${ISODIR}${PS}limine
	COMMAND	${COPY_FILE_COMMAND} ${SOURCE_DIR}${PS}dependencies${PS}limine${PS}limine-bios.sys ${ISODIR}${PS}limine
	COMMAND	xorriso -as mkisofs -b limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image --protective-msdos-label isodir -o ${OUTPUT_DIR}/obos.iso
	COMMAND	${limine_install} bios-install ${OUTPUT_DIR}${PS}obos.iso
	SOURCES ${CMAKE_SOURCE_DIR}/config/limine.cfg ${CMAKE_SOURCE_DIR}/dependencies/limine/limine-uefi-cd.bin ${CMAKE_SOURCE_DIR}/dependencies/limine/limine-bios-cd.bin
            ${CMAKE_SOURCE_DIR}/dependencies/limine/limine-bios-pxe.bin ${CMAKE_SOURCE_DIR}/dependencies/limine/limine-bios.sys
	DEPENDS oboskrnl
	BYPRODUCTS ${ISODIR}/limine.cfg
	BYPRODUCTS ${ISODIR}/EFI/BOOT/BOOTX64.efi
	BYPRODUCTS ${ISODIR}/hyper_iso_boot
	BYPRODUCTS ${ISODIR}/obos/oboskrnl
	BYPRODUCTS ${OUTPUT_DIR}/obos.iso
	# DEPENDS oboskrnl
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	COMMENT "Generating ${OUTPUT_DIR}/obos.iso"
)