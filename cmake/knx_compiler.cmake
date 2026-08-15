# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

# Compiler baseline and warning policy shared by the standalone and ESP-IDF
# builds.  Both modes must agree: the target build is the one that ships, so it
# does not get a laxer warning set than the host build.

set(KNX_MIN_GNU_VERSION 13)
set(KNX_MIN_CLANG_VERSION 17)
set(KNX_MIN_APPLECLANG_VERSION 16.0)
set(KNX_MIN_MSVC_VERSION 19.38)

function(knx_require_cxx23_compiler)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS KNX_MIN_GNU_VERSION)
            message(FATAL_ERROR "KNstaX now requires C++23. GCC ${KNX_MIN_GNU_VERSION}+ is required, found ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS KNX_MIN_CLANG_VERSION)
            message(FATAL_ERROR "KNstaX now requires C++23. Clang ${KNX_MIN_CLANG_VERSION}+ is required, found ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS KNX_MIN_APPLECLANG_VERSION)
            message(FATAL_ERROR "KNstaX now requires C++23. AppleClang ${KNX_MIN_APPLECLANG_VERSION}+ is required, found ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    elseif(MSVC)
        if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS KNX_MIN_MSVC_VERSION)
            message(FATAL_ERROR "KNstaX now requires C++23. MSVC ${KNX_MIN_MSVC_VERSION}+ is required, found ${CMAKE_CXX_COMPILER_VERSION}.")
        endif()
    else()
        message(FATAL_ERROR "Unsupported C++ compiler '${CMAKE_CXX_COMPILER_ID}'. KNstaX currently requires GCC ${KNX_MIN_GNU_VERSION}+, Clang ${KNX_MIN_CLANG_VERSION}+, AppleClang ${KNX_MIN_APPLECLANG_VERSION}+, or MSVC ${KNX_MIN_MSVC_VERSION}+ for C++23 builds.")
    endif()
endfunction()

# The canonical KNstaX warning set.  Applied identically in both build modes.
#
# -Wconversion in particular matters for a protocol stack: silent narrowing
# between the 8/16-bit wire widths and the int-promoted arithmetic around them
# is the exact class of bug that only shows up on unusual address or length
# values.
#
# `vendor_headers` (optional): pass TRUE when the target's translation units
# include third-party headers we do not control.  It drops -Wpedantic, and only
# -Wpedantic.
#
# The reason is specific, not squeamishness: ESP-IDF's own headers legitimately
# use `#include_next` and anonymous structs, both GCC extensions.  ESP-IDF also
# enables -Werror by default, so -Wpedantic on a component that includes those
# headers turns the vendor's code into build failures we cannot fix.  The flags
# that actually catch bugs in our code — -Wconversion and -Wshadow — are clean
# against IDF headers and stay enabled everywhere.
function(knx_apply_warning_flags target)
    set(vendor_headers FALSE)
    if(ARGC GREATER 1)
        set(vendor_headers ${ARGV1})
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        return()
    endif()

    target_compile_options(${target} PRIVATE
        -Wall
        -Wextra
        -Wconversion
        -Wshadow
        -Wno-unused-parameter
    )

    if(NOT vendor_headers)
        target_compile_options(${target} PRIVATE -Wpedantic)
    endif()

    if(KNX_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
