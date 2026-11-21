# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Example toolchain file for KNX_PLATFORM=baremetal on a Cortex-M3.
#
#   cmake -S <product> -B build -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=<KNstaX>/cmake/toolchains/arm-none-eabi.cmake \
#         -DKNX_PLATFORM=baremetal
#
# The core is a default, not a requirement: a product with a different MCU
# sets KNX_ARM_CPU (for example cortex-m0plus, cortex-m4) before this file is
# read, or writes its own toolchain file. Nothing in KNstaX depends on it.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED KNX_ARM_CPU)
    set(KNX_ARM_CPU cortex-m3)
endif()

set(CMAKE_C_COMPILER   arm-none-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-none-eabi-g++)
set(CMAKE_ASM_COMPILER arm-none-eabi-gcc)
set(CMAKE_AR           arm-none-eabi-gcc-ar)
set(CMAKE_RANLIB       arm-none-eabi-gcc-ranlib)

# Compile and link the CMake compiler checks as a static library; there is no
# runtime to link an executable against until the product supplies one.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_knx_arch_flags "-mcpu=${KNX_ARM_CPU} -mthumb")
set(_knx_size_flags "-ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT   "${_knx_arch_flags} ${_knx_size_flags}")
# -fno-threadsafe-statics is C++ only: given to C, GCC warns on every file.
set(CMAKE_CXX_FLAGS_INIT "${_knx_arch_flags} ${_knx_size_flags} -fno-threadsafe-statics -fno-exceptions -fno-rtti")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_knx_arch_flags} --specs=nano.specs -Wl,--gc-sections")

# Use -DCMAKE_BUILD_TYPE=MinSizeRel for -Os. CMake's Release type adds -O3
# after any -Os set here, and the last optimisation flag wins.

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
