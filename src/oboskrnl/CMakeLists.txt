# oboskrnl/CMakeLists.txt

# Copyright (c) 2024 Omar Berrow

list (APPEND oboskrnl_sources 
	"klog.c" "locks/spinlock.c" "irq/irql.c" "scheduler/schedule.c"
	"scheduler/thread.c" "mm/bare_map.c"
)

add_executable(oboskrnl)

target_compile_options(oboskrnl
	PRIVATE $<$<COMPILE_LANGUAGE:C>:${TARGET_COMPILE_OPTIONS_C}>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>
	PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wextra>
)

target_include_directories(oboskrnl 
	PRIVATE "${CMAKE_SOURCE_DIR}/src/oboskrnl"
	PRIVATE ${OBOSKRNL_EXTERNAL_INCLUDES})

target_link_options(oboskrnl
	PRIVATE "-nostdlib"
	PRIVATE "-T" PRIVATE "${CMAKE_SOURCE_DIR}/src/build/x86_64/link.ld"
	PRIVATE ${TARGET_LINKER_OPTIONS}
)

if (DEFINED OBOS_ENABLE_UBSAN)
	list (APPEND oboskrnl_sources "sanitizers/ubsan.c")
	target_compile_options(oboskrnl
		PRIVATE $<$<COMPILE_LANGUAGE:C>:-fsanitize=undefined>
	)
	set(OBOS_ENABLE_UBSAN "1")
endif()
target_compile_definitions(oboskrnl PRIVATE OBOS_UBSAN_ENABLED=$<IF:${OBOS_ENABLE_UBSAN},1,0>)

target_sources(oboskrnl PRIVATE ${oboskrnl_sources})