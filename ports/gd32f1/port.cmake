# SPDX-License-Identifier: GPL-3.0-or-later
# GD32F1 reference port: adds the port to a product's firmware target.
#
#   include(${KNSTAX_DIR}/ports/gd32f1/port.cmake)
#   add_executable(my_device main.cpp)
#   target_include_directories(my_device PRIVATE config)   # gd32f1_port_config.h, if any
#   knx_gd32f1_port(my_device)                             # or LINKER_SCRIPT my.ld
#
# The port's sources are compiled as part of the target, so they see the
# target's include directories (and its gd32f1_port_config.h) and its LTO
# setting. The target must link knx_core and be built with
# cmake/toolchains/arm-none-eabi.cmake (Cortex-M3 by default).

set(KNX_GD32F1_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The toolchain file names objcopy but not size; find it next to the compiler.
if(NOT CMAKE_SIZE)
    get_filename_component(_knx_gd32f1_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    find_program(CMAKE_SIZE NAMES arm-none-eabi-size HINTS "${_knx_gd32f1_bin}" REQUIRED)
endif()

function(knx_gd32f1_port target)
    cmake_parse_arguments(PORT "" "LINKER_SCRIPT" "" ${ARGN})
    if(NOT PORT_LINKER_SCRIPT)
        set(PORT_LINKER_SCRIPT "${KNX_GD32F1_PORT_DIR}/link/gd32f103xb.ld")
    endif()

    target_sources(${target} PRIVATE
        ${KNX_GD32F1_PORT_DIR}/src/gd32f1_startup.c
        ${KNX_GD32F1_PORT_DIR}/src/gd32f1_syscalls.c
        ${KNX_GD32F1_PORT_DIR}/src/gd32f1_system.c
        ${KNX_GD32F1_PORT_DIR}/src/gd32f1_tpuart.c
        ${KNX_GD32F1_PORT_DIR}/src/gd32f1_fmc.c
        ${KNX_GD32F1_PORT_DIR}/src/rtt.c
    )
    target_include_directories(${target} PRIVATE ${KNX_GD32F1_PORT_DIR}/src)
    target_link_options(${target} PRIVATE
        -T${PORT_LINKER_SCRIPT}
        -nostartfiles
        -Wl,-Map=$<TARGET_FILE:${target}>.map
        -Wl,--print-memory-usage
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${PORT_LINKER_SCRIPT}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> $<TARGET_FILE_DIR:${target}>/${target}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
        VERBATIM
    )
endfunction()
