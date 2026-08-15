# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# cmake/knx_product.cmake
# ──────────────────────────────────────────────────────────────────────────────
# knx_commissioned_product(<name>
#   DEFINITION <header>          # path to the product definition header
#   SYMBOL     <symbol>          # constexpr symbol name inside that header
#   [OUTPUT_DIR <dir>]           # output directory (default: CMAKE_CURRENT_BINARY_DIR)
# )
#
# Creates a build-time target <name>_knxprod that:
#   1. Compiles a tiny host-native binary that includes <header> and serialises
#      <symbol>.endpointDefinition to JSON via exportEndpointToJson().
#   2. Runs that binary to produce <name>.json.
#   3. Runs the KNstaX Python exporter to produce <name>.knxprod.xml.
#
# This is the single-source-of-truth bridge: the same C++ type that drives the
# KNX runtime at embedded run time also drives the ETS product catalogue entry
# at build time.  No hand-written .knxprod file is ever required.
#
# Prerequisites (automatically satisfied inside the KNstaX tree):
#   - knx_core CMake target
#   - Python 3 interpreter with tools/knxprod_exporter/exporter.py
#
# Example usage (inside an example or downstream project CMakeLists.txt):
#
#   include(cmake/knx_product.cmake)  # or via find_package(KNstaX)
#
#   knx_commissioned_product(tp1_switch
#       DEFINITION  product.hpp
#       SYMBOL      kSwitchProduct
#   )
#   # Produces: ${CMAKE_CURRENT_BINARY_DIR}/tp1_switch.knxprod.xml
# ──────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.21)

# Locate the tools directory relative to this cmake file.
set(_KNX_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(_KNX_TOOLS_DIR "${_KNX_CMAKE_DIR}/../tools")
set(_KNX_EXPORTER_SCRIPT "${_KNX_TOOLS_DIR}/knxprod_exporter/exporter.py")
set(_KNX_GEN_TEMPLATE "${_KNX_CMAKE_DIR}/knxprod_gen_main.cpp.in")

function(knx_commissioned_product target_name)
    cmake_parse_arguments(ARG "" "DEFINITION;SYMBOL;OUTPUT_DIR" "" ${ARGN})

    if(NOT ARG_DEFINITION)
        message(FATAL_ERROR "knx_commissioned_product(${target_name}): DEFINITION is required")
    endif()
    if(NOT ARG_SYMBOL)
        message(FATAL_ERROR "knx_commissioned_product(${target_name}): SYMBOL is required")
    endif()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()

    # Resolve definition path to absolute
    if(NOT IS_ABSOLUTE "${ARG_DEFINITION}")
        set(ARG_DEFINITION "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_DEFINITION}")
    endif()

    # Paths for generated artefacts
    set(GEN_SRC     "${CMAKE_CURRENT_BINARY_DIR}/${target_name}_knxprod_gen.cpp")
    set(GEN_JSON    "${CMAKE_CURRENT_BINARY_DIR}/${target_name}.json")
    set(GEN_KNXPROD "${ARG_OUTPUT_DIR}/${target_name}.knxprod.xml")

    # Stamp variables used by the .cpp.in template
    set(DEFINITION_ABS_PATH "${ARG_DEFINITION}")
    set(PRODUCT_SYMBOL      "${ARG_SYMBOL}")
    configure_file("${_KNX_GEN_TEMPLATE}" "${GEN_SRC}" @ONLY)

    # Host-native binary that emits JSON
    add_executable(${target_name}_knxprod_gen "${GEN_SRC}")
    target_link_libraries(${target_name}_knxprod_gen PRIVATE knx_core)
    set_target_properties(${target_name}_knxprod_gen PROPERTIES
        # Separate output dir so it doesn't clash with cross-compiled targets
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )

    # Step 1: run the binary → JSON
    add_custom_command(
        OUTPUT  "${GEN_JSON}"
        COMMAND "$<TARGET_FILE:${target_name}_knxprod_gen>" "${GEN_JSON}"
        DEPENDS ${target_name}_knxprod_gen "${ARG_DEFINITION}"
        COMMENT "KNstaX: exporting ${target_name} product definition → JSON"
        VERBATIM
    )

    # Step 2: Python exporter → .knxprod.xml
    find_program(_PYTHON_EXE NAMES python3 python REQUIRED)
    add_custom_command(
        OUTPUT  "${GEN_KNXPROD}"
        COMMAND "${_PYTHON_EXE}"
                "${_KNX_EXPORTER_SCRIPT}"
                "--format" "knxprod"
                "--input"  "${GEN_JSON}"
                "--output" "${GEN_KNXPROD}"
        DEPENDS "${GEN_JSON}" "${_KNX_EXPORTER_SCRIPT}"
        COMMENT "KNstaX: generating ${target_name}.knxprod.xml"
        VERBATIM
    )

    # Top-level target so `cmake --build . --target tp1_switch_knxprod` works,
    # and it is added to ALL so it runs with a plain `cmake --build .`.
    add_custom_target(${target_name}_knxprod ALL
        DEPENDS "${GEN_KNXPROD}"
    )

    # Expose the output path as a target property for downstream use
    set_property(TARGET ${target_name}_knxprod
        PROPERTY KNX_KNXPROD_OUTPUT "${GEN_KNXPROD}"
    )

    message(STATUS "KNstaX: registered knxprod target '${target_name}_knxprod' → ${GEN_KNXPROD}")
endfunction()
