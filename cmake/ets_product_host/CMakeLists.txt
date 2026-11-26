# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# The host build knx_ets_product() runs for a cross-compiled firmware
# (cmake/knx_ets_product.cmake). Not for use on its own: its input is the
# args.cmake that function writes.

cmake_minimum_required(VERSION 3.21)
project(knx_ets_product_host LANGUAGES C CXX)

if(NOT KNX_ETS_PRODUCT_ARGS OR NOT EXISTS "${KNX_ETS_PRODUCT_ARGS}")
    message(FATAL_ERROR "ets_product_host: run by knx_ets_product(), which passes KNX_ETS_PRODUCT_ARGS")
endif()
include("${KNX_ETS_PRODUCT_ARGS}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KNX_ETS_PRODUCT_ARGS}")

# The firmware's configuration imported over host-minimal (what a host build
# cannot take, such as an ESP-IDF medium choice or AES peripheral, is listed),
# then the host adjustments. A fragment may only set what the configuration
# lets it, so the Data Secure part is separate.
set(_overrides "${ETS_KNSTAX_DIR}/configs/ets-export.defconfig")
file(STRINGS "${ETS_FIRMWARE_CONFIG}" _secure REGEX "^CONFIG_KNX_ENABLE_SECURITY=y")
if(_secure)
    list(APPEND _overrides "${ETS_KNSTAX_DIR}/configs/ets-export-secure.defconfig")
endif()
include("${ETS_KNSTAX_DIR}/cmake/knx_config.cmake")
knx_configure(PROFILE host-minimal IMPORT "${ETS_FIRMWARE_CONFIG}" OVERRIDES ${_overrides})
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ETS_KNSTAX_DIR}" knstax EXCLUDE_FROM_ALL)

set(_KNX_ETS_PRODUCT_NESTED TRUE)
include("${ETS_KNSTAX_DIR}/cmake/knx_ets_product.cmake")
set(_views)
if(ETS_VIEWS)
    set(_views VIEWS ${ETS_VIEWS})
endif()
set(_lock)
if(ETS_LOCK)
    set(_lock LOCK "${ETS_LOCK}")
endif()
knx_ets_product(${ETS_NAME}
    DEFINITION "${ETS_DEFINITION}"
    SYMBOL     ${ETS_SYMBOL}
    ${_views}
    ${_lock}
    INCLUDE_DIRECTORIES ${ETS_INCLUDE_DIRECTORIES}
    COMPILE_DEFINITIONS ${ETS_COMPILE_DEFINITIONS}
    OUTPUT_DIR "${ETS_OUTPUT_DIR}"
    PACKER     ${ETS_PACKER}
)
