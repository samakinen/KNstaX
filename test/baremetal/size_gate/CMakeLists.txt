# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Bare-metal size gate: links the KNstaX share of a TP1 end device for
# Cortex-M3 and leaves an ELF for scripts/ci/knstax_size_budget.py.
#
#   cmake -S test/baremetal/size_gate -B build/size_gate -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<KNstaX>/cmake/toolchains/arm-none-eabi.cmake \
#         -DCMAKE_BUILD_TYPE=MinSizeRel
#   cmake --build build/size_gate
#
# This is its own project (not part of the host build) because it needs the
# cross toolchain. KNstaX is added as a subdirectory with KNX_PLATFORM=baremetal.

cmake_minimum_required(VERSION 3.21)
project(KNstaXSizeGate LANGUAGES C CXX)

set(KNX_PLATFORM baremetal CACHE STRING "" FORCE)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

# Release-grade flags: this is the configuration the budget is defined for.
option(KNX_SIZE_GATE_LTO "Link with -flto, as a product would" ON)
set(KNX_SIZE_GATE_LOG_LEVEL 0 CACHE STRING "KNX_LOG_LEVEL_COMPILE for the gate (0 = no log strings)")

get_filename_component(KNX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
add_subdirectory(${KNX_ROOT} knstax EXCLUDE_FROM_ALL)

target_compile_definitions(knx_core PUBLIC KNX_LOG_LEVEL_COMPILE=${KNX_SIZE_GATE_LOG_LEVEL})

# Emulator variant: -DKNX_SIZE_GATE_SEMIHOST=ON links for an address map with code
# at 0 (QEMU mps2-an385) and ends the run by printing the heap high-water mark.
option(KNX_SIZE_GATE_SEMIHOST "Build the run-and-report variant for QEMU" OFF)

add_executable(size_gate
    main.cpp
    startup.c
    syscalls.c
)
target_compile_features(size_gate PRIVATE cxx_std_23)
target_link_libraries(size_gate PRIVATE knx_core)
target_link_options(size_gate PRIVATE
    -T${CMAKE_CURRENT_LIST_DIR}/link.ld
    -nostartfiles
    -Wl,-Map=$<TARGET_FILE:size_gate>.map
)
set_target_properties(size_gate PROPERTIES SUFFIX ".elf")

if(KNX_SIZE_GATE_SEMIHOST)
    target_compile_definitions(size_gate PRIVATE KNX_SIZE_GATE_SEMIHOST=1)
    target_link_options(size_gate PRIVATE -Wl,--defsym=__flash_origin=0 -Wl,--wrap=malloc )
endif()

if(KNX_SIZE_GATE_LTO)
    target_compile_options(knx_core PRIVATE -flto)
    target_compile_options(size_gate PRIVATE -flto)
    target_link_options(size_gate PRIVATE -flto)
endif()
