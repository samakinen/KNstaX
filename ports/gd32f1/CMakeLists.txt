# SPDX-License-Identifier: GPL-3.0-or-later
# The GD32F1 reference port's example: a one-channel KNX switch actuator on a
# GD32F103 or STM32F103 with a TP-UART2. See README.md.
#
#   cmake -S ports/gd32f1 -B build/gd32f1 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake
#   cmake --build build/gd32f1
#
# A product copies this file and the example directory, and points KNSTAX_DIR
# at its KNstaX checkout.

cmake_minimum_required(VERSION 3.21)
project(knx_gd32f1_example LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "" FORCE)
endif()

set(KNSTAX_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH "KNstaX source tree")
option(KNX_GD32F1_LTO "Link with -flto" ON)
option(KNX_GD32F1_WERROR "Treat warnings in the port and the example as errors" OFF)

include(${KNSTAX_DIR}/cmake/knx_config.cmake)
knx_configure(PROFILE tp1-small)
add_subdirectory(${KNSTAX_DIR} knstax EXCLUDE_FROM_ALL)
include(${KNSTAX_DIR}/ports/gd32f1/port.cmake)

add_executable(knx_gd32f1_example example/main.cpp)
target_include_directories(knx_gd32f1_example PRIVATE example)
target_compile_features(knx_gd32f1_example PRIVATE cxx_std_23)
target_compile_options(knx_gd32f1_example PRIVATE -g -Wall -Wextra $<$<BOOL:${KNX_GD32F1_WERROR}>:-Werror>)
target_link_libraries(knx_gd32f1_example PRIVATE knx_core)
knx_gd32f1_port(knx_gd32f1_example)

if(KNX_GD32F1_LTO)
    target_compile_options(knx_core PRIVATE -flto)
    target_compile_options(knx_gd32f1_example PRIVATE -flto)
    target_link_options(knx_gd32f1_example PRIVATE -flto)
endif()
