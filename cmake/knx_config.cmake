# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Build-time configuration for standalone (non-ESP-IDF) builds.
#
# `Kconfig` is the one schema. ESP-IDF reads it through its own tooling; a
# standalone build resolves it here with tools/kconfig/knx_genconfig.py:
#
#   <build>/knx-config/knx/autoconf.h    the CONFIG_KNX_* macros
#   <build>/knx-config/knx_config.cmake  the same values as CMake variables
#   <build>/knx-config/knx.config        the resolved configuration, for reading
#
# A product picks a profile before it adds KNstaX:
#
#   include(<KNstaX>/cmake/knx_config.cmake)
#   knx_configure(PROFILE tp1-small OVERRIDES ${CMAKE_SOURCE_DIR}/knx.defconfig)
#   add_subdirectory(<KNstaX> knstax)
#
# KNstaX's own CMakeLists.txt calls knx_configure() with the default profile
# (tp1-small on KNX_PLATFORM=baremetal, host-test otherwise) when the product
# has not.
#
#   knx_configure(PROFILE <name | file> [IMPORT <config>...] [OVERRIDES <defconfig>...])
#
#   PROFILE    a name under configs/profiles/ (without .defconfig), or a path
#              to a defconfig fragment of the product's own.
#   IMPORT     the configuration of another build (an ESP-IDF sdkconfig's
#              CONFIG_KNX_* lines, a cross build's knx.config), applied after
#              the profile as far as this configuration can take it: settings
#              that do not apply here are listed, not errors. For a host build
#              that has to match a firmware (cmake/knx_ets_product.cmake).
#   OVERRIDES  further fragments, applied after the profile and the imports
#              in the order given.
#
# Legacy command-line options (-DKNX_FEATURE_NETIP=OFF and the like, see
# _KNX_LEGACY_OPTIONS below) still work: each one given becomes an assignment
# in a generated fragment applied after the profile and before OVERRIDES, with
# a deprecation message. Prefer an overrides fragment.
#
# Results, set in the caller's scope: KNX_CONFIG_DIR (the output directory)
# and KNX_PROFILE. The caller includes ${KNX_CONFIG_DIR}/knx_config.cmake.

set(_KNX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(_KNX_ROOT "${_KNX_ROOT}" ABSOLUTE)

# legacy CMake option -> Kconfig symbol (without the CONFIG_ prefix)
set(_KNX_LEGACY_OPTIONS
    KNX_FEATURE_TP1:KNX_BUILD_TP1
    KNX_FEATURE_NETIP:KNX_BUILD_NETIP
    KNX_FEATURE_COUPLER:KNX_ENABLE_COUPLER
    KNX_FEATURE_TUNNELING:KNX_ENABLE_TUNNELING
    KNX_FEATURE_VIRTUAL_BUS:KNX_BUILD_VIRTUAL_BUS
    KNX_FEATURE_ETS_TOOLING:KNX_BUILD_ETS_TOOLING
    KNX_ENABLE_TEST_HOOKS:KNX_BUILD_TEST_HOOKS
    KNX_ZLIB_ENABLED:KNX_BUILD_ZLIB
    KNX_SECURE_ENABLED:KNX_ENABLE_SECURITY
)

function(knx_configure)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "PROFILE" "IMPORT;OVERRIDES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "knx_configure: unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    get_property(_done GLOBAL PROPERTY KNX_CONFIGURED SET)
    if(_done)
        message(FATAL_ERROR "knx_configure: KNstaX is already configured (call it once, before add_subdirectory)")
    endif()
    if(NOT arg_PROFILE)
        message(FATAL_ERROR "knx_configure: PROFILE is required")
    endif()

    # A profile is a name under configs/profiles/ or a file.
    if(EXISTS "${arg_PROFILE}" AND NOT IS_DIRECTORY "${arg_PROFILE}")
        get_filename_component(_profile_file "${arg_PROFILE}" ABSOLUTE)
    else()
        set(_profile_file "${_KNX_ROOT}/configs/profiles/${arg_PROFILE}.defconfig")
        if(NOT EXISTS "${_profile_file}")
            file(GLOB _known "${_KNX_ROOT}/configs/profiles/*.defconfig")
            set(_names)
            foreach(_f IN LISTS _known)
                get_filename_component(_n "${_f}" NAME_WE)
                list(APPEND _names "${_n}")
            endforeach()
            message(FATAL_ERROR "knx_configure: no profile '${arg_PROFILE}' (known: ${_names})")
        endif()
    endif()

    # The generator is a Python script; a cross toolchain may restrict where
    # programs are searched, and Python is a build-host tool.
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    if(Python3_EXECUTABLE)
        set(_py "${Python3_EXECUTABLE}")
    else()
        find_program(KNX_PYTHON NAMES python3 python)
        set(_py "${KNX_PYTHON}")
    endif()
    if(NOT _py)
        message(FATAL_ERROR "knx_configure: Python 3 is needed to resolve the KNstaX configuration")
    endif()

    set(_out "${CMAKE_BINARY_DIR}/knx-config")
    file(MAKE_DIRECTORY "${_out}")

    # Legacy -D options become one generated fragment.
    set(_legacy_text "# Generated from legacy -D options by knx_configure(). Do not edit.\n")
    set(_legacy_names)
    foreach(_pair IN LISTS _KNX_LEGACY_OPTIONS)
        string(REPLACE ":" ";" _p "${_pair}")
        list(GET _p 0 _opt)
        list(GET _p 1 _sym)
        if(DEFINED ${_opt})
            list(APPEND _legacy_names "${_opt}")
            if(${_opt})
                string(APPEND _legacy_text "CONFIG_${_sym}=y\n")
            else()
                string(APPEND _legacy_text "CONFIG_${_sym}=n\n")
            endif()
        endif()
    endforeach()
    if(DEFINED KNX_PLATFORM)
        list(APPEND _legacy_names KNX_PLATFORM)
        string(TOUPPER "${KNX_PLATFORM}" _plat)
        string(APPEND _legacy_text "CONFIG_KNX_STANDALONE_PLATFORM_${_plat}=y\n")
    endif()

    set(_fragments "${_profile_file}")
    set(_imports)
    foreach(_i IN LISTS arg_IMPORT)
        get_filename_component(_i_abs "${_i}" ABSOLUTE)
        list(APPEND _imports "${_i_abs}")
    endforeach()
    if(_legacy_names)
        list(JOIN _legacy_names ", " _legacy_list)
        message(STATUS "KNstaX: deprecated -D options applied over the profile: ${_legacy_list}; "
                       "use knx_configure(PROFILE ... OVERRIDES <defconfig>) instead")
        set(_legacy_file "${_out}/legacy-overrides.defconfig")
        set(_old "")
        if(EXISTS "${_legacy_file}")
            file(READ "${_legacy_file}" _old)
        endif()
        if(NOT _old STREQUAL _legacy_text)
            file(WRITE "${_legacy_file}" "${_legacy_text}")
        endif()
        list(APPEND _fragments "${_legacy_file}")
    endif()
    foreach(_o IN LISTS arg_OVERRIDES)
        get_filename_component(_o_abs "${_o}" ABSOLUTE)
        list(APPEND _fragments "${_o_abs}")
    endforeach()
    # What `knx_menuconfig` saved, if anything: the last word.
    set(_menu_file "${_out}/menuconfig.config")
    if(EXISTS "${_menu_file}")
        message(STATUS "KNstaX: applying ${_menu_file} (from knx_menuconfig)")
        list(APPEND _fragments "${_menu_file}")
    endif()

    set(_gen_args --kconfig "${_KNX_ROOT}/tools/kconfig/Kconfig.standalone.top"
                  --srctree "${_KNX_ROOT}" --out-dir "${_out}")
    # The profile, then the imports, then the rest.
    set(_rest ${_fragments})
    list(POP_FRONT _rest _first)
    list(APPEND _gen_args --fragment "${_first}")
    foreach(_i IN LISTS _imports)
        list(APPEND _gen_args --import "${_i}")
    endforeach()
    foreach(_f IN LISTS _rest)
        list(APPEND _gen_args --fragment "${_f}")
    endforeach()
    execute_process(
        COMMAND "${_py}" -I "${_KNX_ROOT}/tools/kconfig/knx_genconfig.py" ${_gen_args}
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "knx_configure: the KNstaX configuration is not valid\n${_stderr}")
    endif()
    if(_stderr)
        message(WARNING "knx_configure: ${_stderr}")
    endif()
    string(STRIP "${_stdout}" _stdout)
    message(STATUS "${_stdout}")

    # Re-run CMake when any input changes. The generated files are outputs and
    # stay out of this list, or every configure would trigger the next.
    set(_dep_fragments ${_fragments})
    if(_legacy_names)
        list(REMOVE_ITEM _dep_fragments "${_out}/legacy-overrides.defconfig")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${_KNX_ROOT}/Kconfig"
        "${_KNX_ROOT}/Kconfig.standalone"
        "${_KNX_ROOT}/tools/kconfig/Kconfig.standalone.top"
        "${_KNX_ROOT}/tools/kconfig/knx_genconfig.py"
        "${_KNX_ROOT}/tools/kconfig/kconfiglib.py"
        ${_imports} ${_dep_fragments})

    # An editor on the resolved configuration. It saves to menuconfig.config,
    # which the next configure applies last. Needs a terminal with curses.
    if(NOT TARGET knx_menuconfig)
        add_custom_target(knx_menuconfig
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_out}/knx.config" "${_menu_file}"
            COMMAND ${CMAKE_COMMAND} -E env "srctree=${_KNX_ROOT}" "KCONFIG_CONFIG=${_menu_file}"
                    "${_py}" "${_KNX_ROOT}/tools/kconfig/menuconfig.py"
                    "${_KNX_ROOT}/tools/kconfig/Kconfig.standalone.top"
            COMMAND ${CMAKE_COMMAND} -E echo "Saved ${_menu_file}; run cmake again to apply it."
            USES_TERMINAL
            COMMENT "KNstaX configuration editor")
    endif()

    set_property(GLOBAL PROPERTY KNX_CONFIGURED TRUE)
    set(KNX_CONFIG_DIR "${_out}" PARENT_SCOPE)
    set(KNX_PROFILE "${arg_PROFILE}" PARENT_SCOPE)
endfunction()
