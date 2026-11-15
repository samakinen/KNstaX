# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen
#
# Host test registration.
#
#   knx_add_test(<name>
#       SOURCES <file>... | COMMAND <command> [<arg>...]
#       [ARGS <arg>...]
#       [LIBS <lib>...]
#       [NO_UNITY]
#       [LABELS <label>...]
#       [TIMEOUT <seconds>]
#       [WORKING_DIRECTORY <dir>]
#       [ENVIRONMENT <var=value>...]
#       [RUN_SERIAL])
#
# With SOURCES, builds executable <name> from the sources, links it against
# knx_core and the Unity shim (NO_UNITY leaves the shim out, LIBS adds more
# after both), and registers it as test <name>, run with ARGS. With COMMAND,
# registers test <name> to run that command instead and builds nothing; use it
# for the Python and shell tests.
#
# Every test built from SOURCES is also recorded per label, so a target that
# runs a label (the release gate) can depend on exactly the
# executables that carry it:
#
#   knx_tests_with_label(<out-var> <label>)

function(knx_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg
        "NO_UNITY;RUN_SERIAL"
        "TIMEOUT;WORKING_DIRECTORY"
        "SOURCES;COMMAND;ARGS;LIBS;LABELS;ENVIRONMENT;NEEDS")

    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "knx_add_test(${name}): unknown arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    # NEEDS names the optional features (Kconfig symbols without the KNX_
    # prefix, or the group KNX_ENABLE_ stripped: COUPLER, SECURITY,
    # GO_DIAGNOSTICS, FUNCTION_PROPERTIES, PROPERTY_EXT, MEMORY_EXT,
    # NETWORK_PARAMETER, DPT_FLOAT, DPT_TIME, DPT_STRING, DPT_COLOUR,
    # DPT_ACCESS, DPT_CATALOGUE) the test cannot run without. A feature that is
    # off is absent from the build, so the test is not registered at all; the
    # reason goes to knx_tests_not_registered.txt in the build directory and to
    # the configure log, so a profile's gaps are listed rather than silent.
    foreach(feature IN LISTS arg_NEEDS)
        if(feature STREQUAL "DPT_FLOAT" OR feature STREQUAL "DPT_TIME" OR feature STREQUAL "DPT_STRING"
           OR feature STREQUAL "DPT_COLOUR" OR feature STREQUAL "DPT_ACCESS")
            set(_knx_symbol "CONFIG_KNX_${feature}")
        elseif(feature STREQUAL "DPT_CATALOGUE")
            set(_knx_symbol "CONFIG_KNX_ENABLE_DPT_CATALOGUE")
        else()
            set(_knx_symbol "CONFIG_KNX_ENABLE_${feature}")
        endif()
        if(NOT ${_knx_symbol})
            message(STATUS "knx_add_test: ${name} not registered (needs-${feature})")
            file(APPEND "${CMAKE_BINARY_DIR}/knx_tests_not_registered.txt" "${name} needs-${feature}\n")
            return()
        endif()
    endforeach()

    if(arg_SOURCES AND arg_COMMAND)
        message(FATAL_ERROR "knx_add_test(${name}): give SOURCES or COMMAND, not both")
    endif()

    if(arg_SOURCES)
        add_executable(${name} ${arg_SOURCES})
        target_link_libraries(${name} PRIVATE knx_core)
        if(NOT arg_NO_UNITY)
            target_link_libraries(${name} PRIVATE unity)
        endif()
        if(arg_LIBS)
            target_link_libraries(${name} PRIVATE ${arg_LIBS})
        endif()
        add_test(NAME ${name} COMMAND ${name} ${arg_ARGS})
        foreach(label IN LISTS arg_LABELS)
            set_property(GLOBAL APPEND PROPERTY KNX_TEST_TARGETS_${label} ${name})
        endforeach()
    elseif(arg_COMMAND)
        if(arg_LIBS OR arg_NO_UNITY)
            message(FATAL_ERROR "knx_add_test(${name}): LIBS and NO_UNITY need SOURCES")
        endif()
        add_test(NAME ${name} COMMAND ${arg_COMMAND} ${arg_ARGS})
    else()
        message(FATAL_ERROR "knx_add_test(${name}): SOURCES or COMMAND is required")
    endif()

    # One property per call: a property value that is itself a list (LABELS,
    # ENVIRONMENT) cannot share a set_tests_properties() call with others.
    if(arg_LABELS)
        set_tests_properties(${name} PROPERTIES LABELS "${arg_LABELS}")
    endif()
    if(DEFINED arg_TIMEOUT)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${arg_TIMEOUT})
    endif()
    if(arg_WORKING_DIRECTORY)
        set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${arg_WORKING_DIRECTORY}")
    endif()
    if(arg_ENVIRONMENT)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "${arg_ENVIRONMENT}")
    endif()
    if(arg_RUN_SERIAL)
        set_tests_properties(${name} PROPERTIES RUN_SERIAL TRUE)
    endif()
endfunction()

function(knx_tests_with_label out label)
    get_property(targets GLOBAL PROPERTY KNX_TEST_TARGETS_${label})
    set(${out} ${targets} PARENT_SCOPE)
endfunction()
