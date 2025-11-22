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

if (CMAKE_HOST_WIN32)
	set(SUPPRESS_OUTPUT > NUL 2>&1)
elseif(CMAKE_HOST_LINUX)
	set(SUPPRESS_OUTPUT > /dev/null 2>&1)
endif()

if (OBOS_USE_LIMINE)
	set (LIMINE_ENABLED "true")
	set (LIMINE_NOT_ENABLED "false")
	set(BIOS_INSTALL ${limine_exe} bios-install ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT})
	set(BIOS_PRE_XORRISO fallocate -l16M ${ISODIR}/obos/flanterm_buff && cmake -E make_directory ${ISODIR}/limine/ && cmake -E copy ${CMAKE_SOURCE_DIR}/dependencies/limine/limine-bios.sys ${ISODIR}/limine/)
	set(BTLDR_CONF "limine.conf")
	set(xorriso_bios_cd_path "dependencies/limine/limine-bios-cd.bin")
	set(xorriso_uefi_cd_path "dependencies/limine/limine-uefi-cd.bin")
	set(xorriso_uefi_cd_name "limine-uefi-cd.bin")
	set(xorriso_bios_cd_name "limine-bios-cd.bin")
else()
	set(BIOS_INSTALL ${hyper_install} ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT})
	set(BIOS_PRE_XORRISO true)
	set(BTLDR_CONF "hyper.cfg")
	set(xorriso_bios_cd_path "dependencies/hyper/hyper_iso_boot")
	set(xorriso_uefi_cd_path "config/hyper_uefi_boot.bin")
	set(xorriso_uefi_cd_name "hyper_uefi_boot.bin")
	set(xorriso_bios_cd_name "hyper_iso_boot")
endif()

if (NOT EXISTS ${CMAKE_SOURCE_DIR}/config/${BTLDR_CONF})
	message(FATAL_ERROR "No bootloader configuration file detected!")
endif()

add_custom_command(OUTPUT ${OUTPUT_DIR}/obos.iso
	COMMAND ${OBJCOPY} -g ${OUTPUT_DIR}/oboskrnl ${ISODIR}/obos/oboskrnl ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/config/${BTLDR_CONF} ${ISODIR}/ ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/${xorriso_bios_cd_path} ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/${xorriso_uefi_cd_path} ${ISODIR} ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/out/initrd ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND cmake -E copy ${CMAKE_SOURCE_DIR}/config/initrd.tar ${ISODIR}/obos ${SUPPRESS_OUTPUT}
	COMMAND ${BIOS_PRE_XORRISO}
	COMMAND xorriso -as mkisofs -b ${xorriso_bios_cd_name} -no-emul-boot -boot-load-size 4 -boot-info-table --efi-boot ${xorriso_uefi_cd_name} -efi-boot-part --efi-boot-image --protective-msdos-label out/isodir -o ${OUTPUT_DIR}/obos.iso ${SUPPRESS_OUTPUT}
	COMMAND ${BIOS_INSTALL}
	BYPRODUCTS ${ISODIR}/${BTLDR_CONF}
			   ${ISODIR}/${xorriso_bios_cd_name}
			   ${ISODIR}/${xorriso_uefi_cd_name}
			   ${ISODIR}/obos/oboskrnl
			   ${ISODIR}/obos/initrd
			   ${ISODIR}/obos/initrd.tar
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	DEPENDS ${CMAKE_SOURCE_DIR}/config/initrd.tar ${CMAKE_SOURCE_DIR}/config/${BTLDR_CONF} oboskrnl initrd
)
add_custom_target (isogen ALL
	DEPENDS oboskrnl initrd ${OUTPUT_DIR}/obos.iso
)
