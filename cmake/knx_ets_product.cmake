# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# cmake/knx_ets_product.cmake
# ──────────────────────────────────────────────────────────────────────────────
# The ETS product of a firmware, built with the firmware:
#
#   include(<KNstaX>/cmake/knx_ets_product.cmake)
#   knx_ets_product(<name>
#       DEFINITION <header>                 # the product definition the firmware compiles
#       SYMBOL     <symbol>                 # its constexpr product
#       [VIEWS     <symbol>]                # its object views (knx/product/object_views.hpp)
#       [INCLUDE_DIRECTORIES <dir>...]      # what the header needs to compile on the host
#       [COMPILE_DEFINITIONS <def>...]
#       [LOCK      <product.lock>]          # checked-in record of application versions
#       [OUTPUT_DIR <dir>]                  # default: CMAKE_CURRENT_BINARY_DIR
#       [PACKER    auto | none | <backend>] # default: auto
#       [DIST_DIR  <dir>]                   # collect product + firmware + manifest.json
#       [DIST_FILES <file>...]              # the firmware files to collect
#       [DIST_DEPENDS <target>...]          # the targets that make them
#       [KNX_CONFIG <fragment>]             # the firmware's KNstaX configuration (see below)
#   )
#
# Target <name>_ets_product (in ALL) writes to OUTPUT_DIR:
#
#   <name>.json          the product as the firmware sees it; the file to diff
#   <name>.knxprod.xml   self-contained KNX project XML, validated against the
#                        KNX schema when Python has xmlschema
#   <name>.ets.xml       the same without MasterData: the packer's input
#   <name>.knxprod       packed and signed, when a packer is available (on a PC
#                        with ETS); otherwise the build says how to pack it
#
# and, with LOCK, checks the product against its entry in the lock file
# (tools/ets_product/ets_product.py): a product that changed without a new
# application version fails the build, because ETS would refuse it. With
# DIST_DIR, target <name>_dist collects everything into DIST_DIR/<name>/ with
# a manifest.json naming the firmware and KNstaX commits.
#
# Configuration. The product file declares the firmware's capacities, so it is
# built with the firmware's own KNstaX configuration. In a cross build (bare
# metal, ESP-IDF) the exporter has to run on the host, so a nested host build
# does the work: its configuration is host-minimal, then the firmware's resolved
# configuration (KNX_CONFIG; by default <build>/knx-config/knx.config of a
# standalone build, or the CONFIG_KNX_* lines of an ESP-IDF build's sdkconfig),
# then configs/ets-export.defconfig, which makes it a host build without
# changing what the product declares. In a host build the product is built in
# place, with the build's own configuration.
#
# Cache options: KNX_ETS_PRODUCT (ON; OFF skips the product), KNX_ETS_LOCK_MODE
# (check; warn reports a missing version raise without failing), and
# KNX_ETS_HOST_C_COMPILER / KNX_ETS_HOST_CXX_COMPILER for the nested build.
# ──────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.21)

include("${CMAKE_CURRENT_LIST_DIR}/knx_product.cmake")

set(_KNX_ETS_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_KNX_ETS_ROOT "${_KNX_ETS_ROOT}" ABSOLUTE)
set(_KNX_ETS_TOOL "${_KNX_ETS_ROOT}/tools/ets_product/ets_product.py")
set(_KNX_ETS_EXPORTER "${_KNX_ETS_ROOT}/tools/knxprod_exporter/exporter.py")

option(KNX_ETS_PRODUCT "Build the ETS product file with the firmware" ON)
set(KNX_ETS_LOCK_MODE "check" CACHE STRING "Product lock: check (fail on a change without a new version) or warn")
set_property(CACHE KNX_ETS_LOCK_MODE PROPERTY STRINGS check warn)

function(_knx_ets_python out)
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    if(Python3_EXECUTABLE)
        set(${out} "${Python3_EXECUTABLE}" PARENT_SCOPE)
        return()
    endif()
    find_program(KNX_ETS_PYTHON NAMES python3 python)
    if(NOT KNX_ETS_PYTHON)
        message(FATAL_ERROR "knx_ets_product: Python 3 is needed (or -DKNX_ETS_PRODUCT=OFF)")
    endif()
    set(${out} "${KNX_ETS_PYTHON}" PARENT_SCOPE)
endfunction()

function(knx_ets_product name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
        "DEFINITION;SYMBOL;VIEWS;LOCK;OUTPUT_DIR;PACKER;DIST_DIR;KNX_CONFIG"
        "INCLUDE_DIRECTORIES;COMPILE_DEFINITIONS;DIST_FILES;DIST_DEPENDS")
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "knx_ets_product(${name}): unknown arguments: ${ARG_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT KNX_ETS_PRODUCT)
        message(STATUS "KNstaX: ETS product ${name} skipped (KNX_ETS_PRODUCT=OFF)")
        return()
    endif()
    foreach(_req DEFINITION SYMBOL)
        if(NOT ARG_${_req})
            message(FATAL_ERROR "knx_ets_product(${name}): ${_req} is required")
        endif()
    endforeach()
    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    if(NOT ARG_PACKER)
        set(ARG_PACKER auto)
    endif()
    foreach(_path DEFINITION LOCK OUTPUT_DIR DIST_DIR KNX_CONFIG)
        if(ARG_${_path})
            get_filename_component(ARG_${_path} "${ARG_${_path}}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        endif()
    endforeach()
    set(_abs_dirs)
    foreach(_d IN LISTS ARG_INCLUDE_DIRECTORIES)
        get_filename_component(_d "${_d}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        list(APPEND _abs_dirs "${_d}")
    endforeach()
    set(ARG_INCLUDE_DIRECTORIES ${_abs_dirs})
    file(MAKE_DIRECTORY "${ARG_OUTPUT_DIR}")
    _knx_ets_python(_py)

    set(_cross FALSE)
    if(CMAKE_CROSSCOMPILING OR DEFINED IDF_PATH OR COMMAND idf_component_register)
        set(_cross TRUE)
    endif()
    if(_cross AND NOT _KNX_ETS_PRODUCT_NESTED)
        _knx_ets_product_nested(${name})
    else()
        _knx_ets_product_direct(${name})
    endif()

    if(ARG_DIST_DIR)
        set(_file_args)
        foreach(_f IN LISTS ARG_DIST_FILES)
            get_filename_component(_f "${_f}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
            list(APPEND _file_args --file "${_f}")
        endforeach()
        # Always run: it is a copy where anything changed, and the manifest
        # names the commits the files were built from.
        add_custom_target(${name}_dist ALL
            COMMAND "${_py}" "${_KNX_ETS_TOOL}" dist
                    --out "${ARG_DIST_DIR}/${name}" --name ${name}
                    --product-dir "${ARG_OUTPUT_DIR}" ${_file_args}
                    --source-dir "${CMAKE_SOURCE_DIR}" --knstax-dir "${_KNX_ETS_ROOT}"
            COMMENT "KNstaX: collecting ${name} in ${ARG_DIST_DIR}/${name}"
            VERBATIM)
        add_dependencies(${name}_dist ${name}_ets_product ${ARG_DIST_DEPENDS})
    endif()
endfunction()

# The product built in this (host) build, with this build's configuration.
macro(_knx_ets_product_direct name)
    set(_out "${ARG_OUTPUT_DIR}")
    set(_views)
    if(ARG_VIEWS)
        set(_views VIEWS ${ARG_VIEWS})
    endif()
    knx_commissioned_product(${name}
        DEFINITION "${ARG_DEFINITION}" SYMBOL ${ARG_SYMBOL} ${_views} OUTPUT_DIR "${_out}")
    if(ARG_INCLUDE_DIRECTORIES)
        target_include_directories(${name}_knxprod_gen PRIVATE ${ARG_INCLUDE_DIRECTORIES})
    endif()
    if(ARG_COMPILE_DEFINITIONS)
        target_compile_definitions(${name}_knxprod_gen PRIVATE ${ARG_COMPILE_DEFINITIONS})
    endif()

    set(_json_gen "${CMAKE_CURRENT_BINARY_DIR}/${name}.json")
    set(_xml "${_out}/${name}.knxprod.xml")
    set(_ets "${_out}/${name}.ets.xml")
    set(_stamps "${_xml}" "${_ets}")
    if(NOT _out STREQUAL CMAKE_CURRENT_BINARY_DIR)
        add_custom_command(OUTPUT "${_out}/${name}.json"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_json_gen}" "${_out}/${name}.json"
            DEPENDS "${_json_gen}" VERBATIM)
        list(APPEND _stamps "${_out}/${name}.json")
    endif()
    add_custom_command(OUTPUT "${_ets}"
        COMMAND "${_py}" "${_KNX_ETS_EXPORTER}" --format ets --input "${_json_gen}" --output "${_ets}"
        DEPENDS "${_json_gen}" "${_KNX_ETS_EXPORTER}"
        COMMENT "KNstaX: generating ${name}.ets.xml"
        VERBATIM)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.validated"
        COMMAND "${_py}" "${_KNX_ETS_TOOL}" validate --if-available "${_xml}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/${name}.validated"
        DEPENDS "${_xml}" "${_KNX_ETS_TOOL}"
        COMMENT "KNstaX: validating ${name}.knxprod.xml against the KNX schema"
        VERBATIM)
    list(APPEND _stamps "${CMAKE_CURRENT_BINARY_DIR}/${name}.validated")
    if(ARG_LOCK)
        # The lock file is an input and, when the version was raised, an
        # output as well; the next build then finds it up to date.
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.locked"
            COMMAND "${_py}" "${_KNX_ETS_TOOL}" lock --lock "${ARG_LOCK}" --name ${name}
                    --mode ${KNX_ETS_LOCK_MODE} "${_ets}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/${name}.locked"
            DEPENDS "${_ets}" "${_KNX_ETS_TOOL}"
            COMMENT "KNstaX: checking ${name} against ${ARG_LOCK}"
            VERBATIM)
        list(APPEND _stamps "${CMAKE_CURRENT_BINARY_DIR}/${name}.locked")
    endif()
    if(NOT ARG_PACKER STREQUAL "none")
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${name}.packed"
            COMMAND "${_py}" "${_KNX_ETS_TOOL}" pack --packer ${ARG_PACKER} --if-available
                    --output "${_out}/${name}.knxprod" "${_ets}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_CURRENT_BINARY_DIR}/${name}.packed"
            DEPENDS "${_ets}" "${_KNX_ETS_TOOL}"
            BYPRODUCTS "${_out}/${name}.knxprod"
            COMMENT "KNstaX: packing ${name}.knxprod"
            VERBATIM)
        list(APPEND _stamps "${CMAKE_CURRENT_BINARY_DIR}/${name}.packed")
    endif()
    add_custom_target(${name}_ets_product ALL DEPENDS ${_stamps})
endmacro()

# A cross build: the same function, run by a nested host build.
macro(_knx_ets_product_nested name)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/ets_product/${name}")
    file(MAKE_DIRECTORY "${_dir}")

    # The firmware's KNstaX configuration: the resolved configuration of a
    # standalone build, or an ESP-IDF build's sdkconfig.
    if(ARG_KNX_CONFIG)
        set(_source_config "${ARG_KNX_CONFIG}")
    elseif(COMMAND idf_build_get_property)
        idf_build_get_property(_source_config SDKCONFIG)
    elseif(KNX_CONFIG_DIR)
        set(_source_config "${KNX_CONFIG_DIR}/knx.config")
    else()
        set(_source_config "${CMAKE_BINARY_DIR}/knx-config/knx.config")
    endif()
    if(NOT EXISTS "${_source_config}")
        message(FATAL_ERROR "knx_ets_product(${name}): no firmware configuration at "
                            "${_source_config}; call knx_configure() first or pass KNX_CONFIG")
    endif()
    # Its CONFIG_KNX_* lines. Kconfig is one schema, so they are valid for the
    # standalone resolver whichever build wrote them; the rest (CONFIG_IDF_*,
    # the product's own options) is not KNstaX's.
    file(STRINGS "${_source_config}" _lines REGEX "^(# )?CONFIG_KNX_")
    list(JOIN _lines "\n" _text)
    set(_firmware_config "${_dir}/firmware.defconfig")
    set(_text "# The CONFIG_KNX_* lines of ${_source_config}. Generated, do not edit.\n${_text}\n")
    set(_old "")
    if(EXISTS "${_firmware_config}")
        file(READ "${_firmware_config}" _old)
    endif()
    if(NOT _old STREQUAL _text)
        file(WRITE "${_firmware_config}" "${_text}")
    endif()

    # Host compilers: plain names, which a cross toolchain's prefixed
    # binaries never shadow.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    find_program(KNX_ETS_HOST_C_COMPILER NAMES cc gcc clang)
    find_program(KNX_ETS_HOST_CXX_COMPILER NAMES c++ g++ clang++)
    if(NOT KNX_ETS_HOST_C_COMPILER OR NOT KNX_ETS_HOST_CXX_COMPILER)
        message(FATAL_ERROR "knx_ets_product(${name}): the ETS product file is built on the "
                            "host and needs a host C/C++ compiler (C++23). Install one, set "
                            "KNX_ETS_HOST_C_COMPILER and KNX_ETS_HOST_CXX_COMPILER, or skip the "
                            "product with -DKNX_ETS_PRODUCT=OFF.")
    endif()

    # Everything the nested build needs, as one file: lists and paths pass
    # through intact, and a change re-runs the nested configure.
    set(_args "${_dir}/args.cmake")
    set(_text "# Generated by knx_ets_product(${name}). Do not edit.\n")
    foreach(_v IN ITEMS DEFINITION SYMBOL VIEWS LOCK OUTPUT_DIR PACKER INCLUDE_DIRECTORIES COMPILE_DEFINITIONS)
        string(APPEND _text "set(ETS_${_v} [==[${ARG_${_v}}]==])\n")
    endforeach()
    string(APPEND _text "set(ETS_NAME [==[${name}]==])\n"
                        "set(ETS_KNSTAX_DIR [==[${_KNX_ETS_ROOT}]==])\n"
                        "set(ETS_FIRMWARE_CONFIG [==[${_firmware_config}]==])\n"
                        "set(KNX_ETS_LOCK_MODE [==[${KNX_ETS_LOCK_MODE}]==])\n"
                        "set(Python3_EXECUTABLE [==[${_py}]==])\n")
    set(_old "")
    if(EXISTS "${_args}")
        file(READ "${_args}" _old)
    endif()
    if(NOT _old STREQUAL _text)
        file(WRITE "${_args}" "${_text}")
    endif()

    set(_host_project "${_KNX_ETS_ROOT}/cmake/ets_product_host")
    add_custom_command(
        OUTPUT  "${_dir}/build/CMakeCache.txt"
        COMMAND "${CMAKE_COMMAND}" -S "${_host_project}" -B "${_dir}/build"
                -G "${CMAKE_GENERATOR}"
                -DCMAKE_BUILD_TYPE=Release
                "-DCMAKE_C_COMPILER=${KNX_ETS_HOST_C_COMPILER}"
                "-DCMAKE_CXX_COMPILER=${KNX_ETS_HOST_CXX_COMPILER}"
                "-DKNX_ETS_PRODUCT_ARGS=${_args}"
        DEPENDS "${_args}" "${_host_project}/CMakeLists.txt"
        COMMENT "KNstaX: configuring the host build of the ETS product ${name}"
        VERBATIM)
    # Always invoked; the nested build decides whether anything is to do.
    # USES_TERMINAL keeps it out of the outer build's job slots.
    add_custom_target(${name}_ets_product ALL
        COMMAND "${CMAKE_COMMAND}" --build "${_dir}/build" --target ${name}_ets_product
        DEPENDS "${_dir}/build/CMakeCache.txt"
        COMMENT "KNstaX: ETS product ${name} -> ${ARG_OUTPUT_DIR}"
        USES_TERMINAL
        VERBATIM)
endmacro()
