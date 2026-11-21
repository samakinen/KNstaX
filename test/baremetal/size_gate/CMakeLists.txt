# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Bare-metal size gate: links the KNstaX share of a TP1 end device and leaves
# an ELF for scripts/ci/knstax_size_gate.py. One reference device per profile
# and core (small-target-profile.md section 7):
#
#   cmake -S test/baremetal/size_gate -B build/size_gate -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<KNstaX>/cmake/toolchains/arm-none-eabi.cmake \
#         -DCMAKE_BUILD_TYPE=MinSizeRel \
#         [-DKNX_SIZE_GATE_PROFILE=tp1-standard] [-DKNX_ARM_CPU=cortex-m4] \
#         [-DKNX_SIZE_GATE_VARIANT=secure]
#   cmake --build build/size_gate
#
# The profile defaults to tp1-small; the core is the toolchain file's
# KNX_ARM_CPU (cortex-m3 unless set). run_gate.sh does all of this and picks
# the budget file.
#
# This is its own project (not part of the host build) because it needs the
# cross toolchain. KNstaX is configured with the profile (baremetal, TP1 only,
# log level 0 = no log strings) and added as a subdirectory.

cmake_minimum_required(VERSION 3.21)
project(KNstaXSizeGate LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

# Release-grade flags: this is the configuration the budget is defined for.
option(KNX_SIZE_GATE_LTO "Link with -flto, as a product would" ON)

# The KNstaX tree to measure: this one unless told otherwise. CI points it at
# a checkout of the base of a pull request to get the "before" figures with
# the same reference device.
set(KNX_SIZE_GATE_KNSTAX_ROOT "" CACHE PATH "KNstaX source tree to measure (default: this one)")
if(KNX_SIZE_GATE_KNSTAX_ROOT)
    get_filename_component(KNX_ROOT ${KNX_SIZE_GATE_KNSTAX_ROOT} ABSOLUTE)
else()
    get_filename_component(KNX_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
endif()
include(${KNX_ROOT}/cmake/knx_config.cmake)
set(KNX_SIZE_GATE_PROFILE "tp1-small" CACHE STRING "KNstaX profile of the reference device")
# A variant is the same device with a fragment of variants/ over the profile
# (secure: Data Secure on). Empty: the profile as it is.
set(KNX_SIZE_GATE_VARIANT "" CACHE STRING "Variant of the reference device (a file under variants/, without .defconfig)")
if(KNX_SIZE_GATE_VARIANT)
    set(_gate_variant_file "${CMAKE_CURRENT_LIST_DIR}/variants/${KNX_SIZE_GATE_VARIANT}.defconfig")
    if(NOT EXISTS "${_gate_variant_file}")
        message(FATAL_ERROR "size gate: no variant ${_gate_variant_file}")
    endif()
    knx_configure(PROFILE ${KNX_SIZE_GATE_PROFILE} OVERRIDES "${_gate_variant_file}")
else()
    knx_configure(PROFILE ${KNX_SIZE_GATE_PROFILE})
endif()
message(STATUS "size gate: profile ${KNX_SIZE_GATE_PROFILE}, variant '${KNX_SIZE_GATE_VARIANT}', cpu ${KNX_ARM_CPU}")
add_subdirectory(${KNX_ROOT} knstax EXCLUDE_FROM_ALL)

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
