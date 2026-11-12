# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/knx_compiler.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/knx_sources.cmake")

option(KNX_WARNINGS_AS_ERRORS "Treat KNstaX compiler warnings as errors" OFF)

# The stack version has one source: the `version` field of idf_component.yml,
# which the ESP-IDF component manager publishes. KNX_VERSION_STRING is the full
# string (KNX_STACK_VERSION in code); the numeric part is the CMake project
# version.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/idf_component.yml" _knx_version_line
     REGEX "^version:" LIMIT_COUNT 1)
string(REGEX REPLACE "^version:[ \t]*\"?([^\" \t]+)\"?.*$" "\\1"
       KNX_VERSION_STRING "${_knx_version_line}")
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" KNX_VERSION_NUMERIC "${KNX_VERSION_STRING}")
if(NOT KNX_VERSION_NUMERIC)
    message(FATAL_ERROR "KNstaX: no usable version in idf_component.yml ('${_knx_version_line}')")
endif()

# ============================================================================
# Dual-Mode CMakeLists: Works with ESP-IDF or as standalone CMake project
# ============================================================================

# Check if we're being used in ESP-IDF context
if(DEFINED IDF_PATH OR COMMAND idf_component_register)
    # ========================================================================
    # ESP-IDF Build Mode
    # ========================================================================
    
    # ------------------------------------------------------------------
    # Feature selection (ESP-IDF): driven by Kconfig, not by wishful thinking.
    # CONFIG_* are already in scope here, so a medium that is not selected
    # never contributes a translation unit.
    # ------------------------------------------------------------------
    set(KNX_FEATURE_NETIP 0)
    if(CONFIG_KNX_MEDIUM_IP)
        set(KNX_FEATURE_NETIP 1)
    endif()

    set(KNX_FEATURE_TP1 0)
    if(CONFIG_KNX_MEDIUM_TP1 OR NOT KNX_FEATURE_NETIP)
        set(KNX_FEATURE_TP1 1)
    endif()

    set(KNX_FEATURE_SECURE 0)
    if(CONFIG_KNX_ENABLE_SECURITY)
        set(KNX_FEATURE_SECURE 1)
    endif()

    set(KNX_COMPONENT_SOURCES ${KNX_SOURCES_CORE} ${KNX_SOURCES_PLATFORM_ESPIDF})

    if(KNX_FEATURE_TP1)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_TP1})
    endif()
    if(KNX_FEATURE_NETIP)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_NETIP})
        if(CONFIG_KNX_ENABLE_TUNNELING)
            list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_NETIP_CLIENT})
        endif()
    endif()
    if(KNX_FEATURE_TP1 AND KNX_FEATURE_NETIP)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_TP1_NETIP_BRIDGE})
    endif()
    # The virtual TP1 bus (KNX_SOURCES_VIRTUAL) is a host-test facility and is
    # never part of a component build.

    # KNX Secure on ESP-IDF.  The crypto primitives call into mbedTLS from the
    # IDF, but they are still translation units of their own: without them the
    # Data Secure session (CCM protect/unprotect) has no definition and the
    # stack links only because every `#if KNX_SECURE_ENABLED` guard is off.
    # The IP Secure transport additionally requires the KNXnet/IP group.
    if(KNX_FEATURE_SECURE)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_SECURE_DATA})
        if(KNX_FEATURE_NETIP)
            list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_SECURE_ROUTING})
            # Only a tunnelling client needs the session machinery, and only
            # the session machinery needs X25519 and PBKDF2. Keeping them out
            # of a routing-only device is not just size: they call legacy
            # mbedTLS entry points that mbedTLS 4 — what recent ESP-IDF ships —
            # no longer provides.
            if(CONFIG_KNX_ENABLE_TUNNELING)
                list(APPEND KNX_COMPONENT_SOURCES
                    ${KNX_SOURCES_SECURE_IP_CRYPTO}
                    ${KNX_SOURCES_SECURE_TUNNELING})
            endif()
        endif()
    endif()

    idf_component_register(
        SRCS
            ${KNX_COMPONENT_SOURCES}

        INCLUDE_DIRS
            "include"
            
        REQUIRES
            driver
            esp_driver_gpio
            esp_driver_gptimer
            esp_timer
            esp_event
            esp_netif
            esp_wifi
            lwip
            nvs_flash
            mbedtls
    )
    
    knx_require_cxx23_compiler()

    set_target_properties(${COMPONENT_LIB} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    
    # KNX_FEATURE_NETIP is PUBLIC: application code includes bau.hpp and must
    # see the same media surface the library was compiled with.  The same holds
    # for KNX_SECURE_ENABLED and KNX_FEATURE_TP1: every `#if KNX_SECURE_ENABLED`
    # guard in the stack (Data Secure wiring in bau.cpp included) evaluates to 0
    # when the macro is not defined, so leaving it out of the ESP-IDF branch
    # silently compiled KNX Secure out of the firmware even with
    # CONFIG_KNX_ENABLE_SECURITY=y.
    set(KNX_FEATURE_IP_TUNNELING 0)
    if(KNX_FEATURE_NETIP AND CONFIG_KNX_ENABLE_TUNNELING)
        set(KNX_FEATURE_IP_TUNNELING 1)
    endif()
    set(KNX_FEATURE_SECURE_IP_TUNNELING 0)
    if(KNX_FEATURE_SECURE AND KNX_FEATURE_IP_TUNNELING)
        set(KNX_FEATURE_SECURE_IP_TUNNELING 1)
    endif()

    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        KNX_FEATURE_NETIP=${KNX_FEATURE_NETIP}
        KNX_FEATURE_TP1=${KNX_FEATURE_TP1}
        KNX_SECURE_ENABLED=${KNX_FEATURE_SECURE}
        # PUBLIC for the same reason as the others: application code includes
        # bau.hpp, and a header that disagrees with the library about whether
        # the (secure) tunnelling entry points exist produces a link error
        # rather than a compile one.
        KNX_IP_TUNNELING=${KNX_FEATURE_IP_TUNNELING}
        KNX_SECURE_IP_TUNNELING=${KNX_FEATURE_SECURE_IP_TUNNELING}
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KNX_STACK_VERSION="${KNX_VERSION_STRING}"
        # The mock TP1 physical and the stack-port constructor that takes it
        # exist for host tests only.
        KNX_ENABLE_TEST_HOOKS=0
    )

    # TRUE: this component includes ESP-IDF headers, which are not -Wpedantic clean.
    knx_apply_warning_flags(${COMPONENT_LIB} TRUE)

    # The TP1 bit-bang backend puts its timer/GPIO ISR in IRAM. On Xtensa the
    # assembler otherwise emits each function's constant pool into a separate
    # .literal section, and for an IRAM-resident function the linker then
    # rejects the l32r that reaches it:
    #
    #   dangerous relocation: l32r: literal placed after use
    #
    # -mtext-section-literals interleaves the pool into .text so the literal
    # always precedes its use. RISC-V targets have no l32r and no literal
    # sections, so this is Xtensa-only.
    #
    # Without it the bit-bang backend does not link at all in a debug build —
    # which went unnoticed until CI started building it.
    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        target_compile_options(${COMPONENT_LIB} PRIVATE -mtext-section-literals)
    endif()

else()
    # ========================================================================
    # Standalone CMake Mode
    # ========================================================================
    
    cmake_minimum_required(VERSION 3.21)
    project(KNstaX VERSION ${KNX_VERSION_NUMERIC} LANGUAGES C CXX)
    option(KNX_SECURE_ENABLED "Enable KNX Secure features (mbedTLS AES + PBKDF2 required)" ON)
    option(KNX_BUILD_EXAMPLES "Build standalone examples" OFF)
    option(KNX_ZLIB_ENABLED "Enable optional zlib/DEFLATE support for the KNXprog importer" ON)

    include(CheckIncludeFileCXX)
    check_include_file_cxx("mbedtls/aes.h" HAVE_MBEDTLS_AES)
    check_include_file_cxx("mbedtls/md.h" HAVE_MBEDTLS_MD)
    check_include_file_cxx("mbedtls/pkcs5.h" HAVE_MBEDTLS_PKCS5)

    if(HAVE_MBEDTLS_AES)
        find_library(MBEDTLS_LIBRARY mbedtls)
        find_library(MBEDCRYPTO_LIBRARY mbedcrypto)
    endif()
    # When KNX Secure is enabled, mandate a real crypto backend
    if(KNX_SECURE_ENABLED)
        if(NOT HAVE_MBEDTLS_AES)
            message(FATAL_ERROR "KNX_SECURE_ENABLED=ON requires mbedTLS AES headers (mbedtls/aes.h). Install mbedtls development headers, or set -DKNX_SECURE_ENABLED=OFF.")
        endif()

        if(NOT HAVE_MBEDTLS_PKCS5 OR NOT HAVE_MBEDTLS_MD)
            message(FATAL_ERROR "KNX_SECURE_ENABLED=ON requires mbedTLS PBKDF2 headers (mbedtls/pkcs5.h, mbedtls/md.h). Install libmbedtls-dev or provide headers.")
        endif()

        if(NOT (MBEDTLS_LIBRARY AND MBEDCRYPTO_LIBRARY))
            message(FATAL_ERROR "KNX_SECURE_ENABLED=ON requires mbedtls and mbedcrypto libraries. Install libmbedtls-dev, or provide library paths, or set -DKNX_SECURE_ENABLED=OFF.")
        endif()
    endif()

    knx_require_cxx23_compiler()

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    # ========================================================================
    # Platform Selection
    # ========================================================================

    # Detect platform if not specified
    if(NOT DEFINED KNX_PLATFORM)
        if(WIN32 OR CYGWIN)
            set(KNX_PLATFORM "windows")
        elseif(APPLE)
            set(KNX_PLATFORM "macos")
        elseif(UNIX AND NOT APPLE)
            set(KNX_PLATFORM "linux")
        else()
            message(FATAL_ERROR "Unknown platform. Please specify -DKNX_PLATFORM=linux|macos|windows")
        endif()
    endif()

    message(STATUS "Building KNX Stack for platform: ${KNX_PLATFORM}")
    message(STATUS "")
    message(STATUS "Crypto Backends:")
    message(STATUS "  mbedtls/aes.h:      ${HAVE_MBEDTLS_AES}")
    message(STATUS "  mbedtls/md.h:       ${HAVE_MBEDTLS_MD}")
    message(STATUS "  mbedtls/pkcs5.h:    ${HAVE_MBEDTLS_PKCS5}")
    message(STATUS "  mbedtls library:    ${MBEDTLS_LIBRARY}")
    message(STATUS "  mbedcrypto library: ${MBEDCRYPTO_LIBRARY}")
    message(STATUS "  KNX Secure enabled: ${KNX_SECURE_ENABLED}")

    # ========================================================================
    # Source Files - Common
    # ========================================================================

    # ------------------------------------------------------------------
    # Feature selection (standalone/host).
    #
    # Host builds default to every medium ON because the test suite exercises
    # TP1, KNXnet/IP and the couplers between them.  A host build can still
    # opt out (-DKNX_FEATURE_NETIP=OFF) to reproduce a target-shaped link and
    # verify that the TP1-only configuration really is self-contained.
    #
    # The shape the Habinari knxip image ships (TP1 off, KNXnet/IP and Secure
    # on, no tunnelling client) is:
    #   -DKNX_FEATURE_TP1=OFF -DKNX_FEATURE_TUNNELING=OFF
    #   -DKNX_FEATURE_VIRTUAL_BUS=OFF -DKNX_ENABLE_TEST_HOOKS=OFF -DBUILD_TESTS=OFF
    # ------------------------------------------------------------------
    option(KNX_FEATURE_TP1 "Build the TP1 medium" ON)
    option(KNX_FEATURE_NETIP "Build the KNXnet/IP medium" ON)
    option(KNX_FEATURE_TUNNELING "Build the KNXnet/IP client role (tunnelling, secure sessions); CONFIG_KNX_ENABLE_TUNNELING on ESP-IDF" ON)
    option(KNX_FEATURE_VIRTUAL_BUS "Build the virtual TP1 bus simulator" ON)
    option(KNX_FEATURE_ETS_TOOLING "Build the ETS import/migration tooling" ON)
    option(KNX_ENABLE_TEST_HOOKS "Build the mock TP1 physical entry points the host tests use" ON)

    set(COMMON_SOURCES ${KNX_SOURCES_CORE})

    if(KNX_FEATURE_TP1)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_TP1})
    endif()
    if(KNX_FEATURE_NETIP)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_NETIP})
        if(KNX_FEATURE_TUNNELING)
            list(APPEND COMMON_SOURCES ${KNX_SOURCES_NETIP_CLIENT})
        endif()
    endif()
    if(KNX_FEATURE_TP1 AND KNX_FEATURE_NETIP)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_TP1_NETIP_BRIDGE})
    endif()
    if(KNX_FEATURE_VIRTUAL_BUS)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_VIRTUAL})
    endif()
    if(KNX_FEATURE_ETS_TOOLING)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_ETS})
    endif()

    # Optional Security (requires mbedTLS when enabled)
    if(KNX_SECURE_ENABLED)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_SECURE})
        if(KNX_FEATURE_NETIP)
            list(APPEND COMMON_SOURCES ${KNX_SOURCES_SECURE_ROUTING})
            if(KNX_FEATURE_TUNNELING)
                list(APPEND COMMON_SOURCES ${KNX_SOURCES_SECURE_TUNNELING})
            endif()
        endif()
    endif()


    # ========================================================================
    # Platform-Specific Sources
    # ========================================================================

    set(PLATFORM_SOURCES)

    if(KNX_PLATFORM STREQUAL "linux")
        list(APPEND PLATFORM_SOURCES ${KNX_SOURCES_PLATFORM_LINUX})
        set(PLATFORM_LIBS pthread)
    elseif(KNX_PLATFORM STREQUAL "macos")
        set(PLATFORM_LIBS pthread)
    elseif(KNX_PLATFORM STREQUAL "windows")
        # Platform stubs for host builds do not require additional sources
        set(PLATFORM_LIBS)
    else()
        message(FATAL_ERROR "Unknown platform: ${KNX_PLATFORM}")
    endif()

    if(KNX_ZLIB_ENABLED)
        find_package(ZLIB QUIET)
    else()
        set(ZLIB_FOUND OFF)
    endif()

    if(ZLIB_FOUND)
        message(STATUS "ZLIB found: enabling KNXprog DEFLATE support")
        set(KNX_HAS_ZLIB 1)
    else()
        if(KNX_ZLIB_ENABLED)
            message(STATUS "ZLIB not found: KNXprog DEFLATE support disabled.")
        else()
            message(STATUS "KNX_ZLIB_ENABLED=OFF: KNXprog DEFLATE support disabled.")
        endif()
        set(KNX_HAS_ZLIB 0)
    endif()

    # ========================================================================
    # Create Library
    # ========================================================================

    add_library(knx_core STATIC
        ${COMMON_SOURCES}
        ${PLATFORM_SOURCES}
    )

    target_include_directories(knx_core PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(knx_core PUBLIC
        ${PLATFORM_LIBS}
    )

    if(KNX_HAS_ZLIB)
        target_link_libraries(knx_core PUBLIC ZLIB::ZLIB)
    endif()

    if(KNX_SECURE_ENABLED)
        target_link_libraries(knx_core PUBLIC
            ${MBEDCRYPTO_LIBRARY}
            ${MBEDTLS_LIBRARY}
        )
    endif()

    target_compile_definitions(knx_core PUBLIC
        KNX_STACK_VERSION="${KNX_VERSION_STRING}"
        KNX_SECURE_ENABLED=$<IF:$<BOOL:${KNX_SECURE_ENABLED}>,1,0>
        # The client role and secure tunnelling follow the tunnelling switch,
        # as on ESP-IDF, so a host build can reproduce a routing-only
        # (knxip-shaped) device.
        KNX_IP_TUNNELING=$<IF:$<AND:$<BOOL:${KNX_FEATURE_NETIP}>,$<BOOL:${KNX_FEATURE_TUNNELING}>>,1,0>
        KNX_SECURE_IP_TUNNELING=$<IF:$<AND:$<BOOL:${KNX_SECURE_ENABLED}>,$<BOOL:${KNX_FEATURE_NETIP}>,$<BOOL:${KNX_FEATURE_TUNNELING}>>,1,0>
        KNX_HAS_ZLIB=$<IF:$<BOOL:${KNX_HAS_ZLIB}>,1,0>
        KNX_FEATURE_NETIP=$<IF:$<BOOL:${KNX_FEATURE_NETIP}>,1,0>
        KNX_FEATURE_TP1=$<IF:$<BOOL:${KNX_FEATURE_TP1}>,1,0>
        KNX_ENABLE_TEST_HOOKS=$<IF:$<BOOL:${KNX_ENABLE_TEST_HOOKS}>,1,0>
    )

    knx_apply_warning_flags(knx_core)

    # ========================================================================
    # SDK Contract Targets
    # ========================================================================

    set(KNX_SDK_PUBLIC_ENTRY_HEADERS
        knx/product/endpoint.hpp
        knx/product/endpoint_definition.hpp
        knx/product/endpoint_bindings.hpp
        knx/product/endpoint_runtime.hpp
        knx/product/commissioned_product.hpp
        knx/product/commissioned_product_expert.hpp
        knx/product/endpoint_export.hpp
        knx/product/exporter.hpp
        knx/product/fault_model.hpp
    )

    set(KNX_SDK_PUBLIC_HEADER_CONTRACT_DIR ${CMAKE_CURRENT_BINARY_DIR}/sdk_contracts/public_headers)
    file(MAKE_DIRECTORY ${KNX_SDK_PUBLIC_HEADER_CONTRACT_DIR})

    set(KNX_SDK_PUBLIC_HEADER_CONTRACT_SOURCES)
    foreach(public_header IN LISTS KNX_SDK_PUBLIC_ENTRY_HEADERS)
        string(REPLACE "/" "__" contract_name ${public_header})
        string(REPLACE ".hpp" "_contract.cpp" contract_name ${contract_name})
        set(contract_source ${KNX_SDK_PUBLIC_HEADER_CONTRACT_DIR}/${contract_name})
        file(GENERATE OUTPUT ${contract_source} CONTENT "#include \"${public_header}\"\n")
        list(APPEND KNX_SDK_PUBLIC_HEADER_CONTRACT_SOURCES ${contract_source})
    endforeach()

    add_library(knx_sdk_public_header_contract_objects OBJECT
        ${KNX_SDK_PUBLIC_HEADER_CONTRACT_SOURCES}
    )
    target_link_libraries(knx_sdk_public_header_contract_objects PRIVATE
        knx_core
    )

    add_custom_target(knx_sdk_public_header_contracts
        DEPENDS knx_sdk_public_header_contract_objects
    )

    # ========================================================================
    # Tests
    # ========================================================================

    option(BUILD_TESTS "Build and register the test suite (enable/disable all tests)" ON)
    if(BUILD_TESTS)
        enable_testing()
    else()
        message(STATUS "BUILD_TESTS=OFF: skipping test targets and registration")
    endif()

    # Create a minimal Unity implementation from source
    add_library(unity INTERFACE)
    target_include_directories(unity INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/test/unity_mock
    )

    # The product export module (knx_commissioned_product()) is used by the
    # examples as well as the tests, so it is loaded whether or not tests are.
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/knx_product.cmake)

    # Everything from here to the examples is test-only: test executables,
    # the exporter and interop peers they drive. BUILD_TESTS=OFF skips it all.
    if(BUILD_TESTS)
        include(${CMAKE_CURRENT_LIST_DIR}/cmake/knx_testing.cmake)

        # The exporter and interop tests are Python scripts.
        find_package(Python3 COMPONENTS Interpreter REQUIRED)

        # Shorthands for the label sets most tests carry.
        set(TP1_VIRTUAL_UNIT tp1 unit virtual tp1-virtual)

        # Tests that read test/vectors/... by relative path run from the
        # source tree.
        set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})

        # Labels are what `ctest -L` selects on: unit, integration,
        # conformance, script, tp1, tp1-virtual, virtual, physical, secure,
        # Long, benchmark and sdk-public-surface (the release gate).
        # Tests without a label run only in the full suite.

        knx_add_test(test_types SOURCES test/unit/test_types.cpp LABELS unit)
        knx_add_test(test_dpt SOURCES test/unit/test_dpt.cpp LABELS unit)
        knx_add_test(test_telegram_rate_limiter SOURCES test/unit/test_telegram_rate_limiter.cpp)
        knx_add_test(test_group_object_transmit_policy SOURCES test/unit/test_group_object_transmit_policy.cpp)
        knx_add_test(test_group_object_flags SOURCES test/unit/test_group_object_flags.cpp LABELS unit)
        knx_add_test(test_function_property_services SOURCES test/unit/test_function_property_services.cpp LABELS unit)
        knx_add_test(test_network_parameter_service SOURCES test/unit/test_network_parameter_service.cpp LABELS unit)
        knx_add_test(test_endpoint_semantics SOURCES test/unit/test_endpoint_semantics.cpp LABELS unit)
        knx_add_test(test_routing_table_control SOURCES test/unit/test_routing_table_control.cpp LABELS unit)
        knx_add_test(test_coupler_routing SOURCES test/unit/test_coupler_routing.cpp LABELS unit)
        knx_add_test(test_bau_router_role SOURCES test/integration/test_bau_router_role.cpp LABELS integration)
        knx_add_test(test_persistence_schema_version SOURCES test/integration/test_persistence_schema_version.cpp
            LABELS integration RUN_SERIAL)
        knx_add_test(test_link_monitor SOURCES test/unit/test_link_monitor.cpp LABELS physical unit)
        knx_add_test(test_tp1_frame_golden SOURCES test/unit/test_tp1_frame_golden.cpp LABELS tp1 unit)
        knx_add_test(test_frame_encoding SOURCES test/unit/test_frame_encoding.cpp LABELS unit)

        # The reference products the exporter tests render. Built into the source
        # tree because tools/knxprod_exporter/run_exporter.sh runs it from there;
        # it used to be a committed binary, which went stale the moment the
        # descriptor types moved and could not be rebuilt from anything in-tree.
        add_executable(product_exporter tools/product_exporter/main.cpp)
        target_link_libraries(product_exporter PRIVATE knx_core)
        set_target_properties(product_exporter PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools/product_exporter)

        # knxprod exporter: the two that run the wrapper share its output
        # directory, so they run serially and write into the build tree.
        set(KNXPROD_EXPORTER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/knxprod_exporter)
        set(KNXPROD_EXPORTER_ENV "KNXPROD_EXPORTER_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/knxprod_exporter_artifacts")
        # Runs the wrapper, then basic XML checks.
        knx_add_test(test_knxprod_exporter
            COMMAND ${Python3_EXECUTABLE} ${KNXPROD_EXPORTER_DIR}/test_validate_knxprod.py
            RUN_SERIAL ENVIRONMENT ${KNXPROD_EXPORTER_ENV})
        # Runs the wrapper, then XSD validation via xmlschema.
        knx_add_test(test_knxprod_xsd_validation
            COMMAND ${Python3_EXECUTABLE} ${KNXPROD_EXPORTER_DIR}/test_validate_knxprod_xsd.py
            RUN_SERIAL ENVIRONMENT ${KNXPROD_EXPORTER_ENV})
        # Parameter UX and load procedure (pure Python, no wrapper).
        knx_add_test(test_exporter_parameter_ux
            COMMAND ${Python3_EXECUTABLE} ${KNXPROD_EXPORTER_DIR}/test_exporter_parameter_ux.py
            LABELS script)
        # Medium, mask version and master-data ids in the catalogue entry (pure Python).
        knx_add_test(test_exporter_medium
            COMMAND ${Python3_EXECUTABLE} ${KNXPROD_EXPORTER_DIR}/test_exporter_medium.py
            LABELS script)

        knx_add_test(test_frame_codec_errors SOURCES test/unit/test_frame_codec_errors.cpp LABELS unit)
        # Decoder fuzz/robustness
        knx_add_test(test_decoder_robustness SOURCES test/unit/test_decoder_robustness.cpp LABELS unit)
        knx_add_test(test_tp1_integration SOURCES test/integration/test_tp1_integration.cpp LABELS tp1 integration)

        # PoC: generate product profile header from a .knxprod file
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated_product_profile.hpp
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/knxprod2profile.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/examples/sample.knxprod.xml --output ${CMAKE_CURRENT_BINARY_DIR}/generated_product_profile.hpp
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/knxprod2profile.py ${CMAKE_CURRENT_SOURCE_DIR}/templates/profile_header.tpl ${CMAKE_CURRENT_SOURCE_DIR}/tools/examples/sample.knxprod.xml
        )
        add_custom_target(generate_product_profile DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/generated_product_profile.hpp)

        knx_add_test(test_result SOURCES test/unit/test_result.cpp LABELS unit)
        knx_add_test(test_config_validation SOURCES test/unit/test_config_validation.cpp LABELS unit)
        knx_add_test(test_exporter SOURCES test/unit/test_exporter.cpp)

        # Product model: definition/bindings, compiler, export bridge, the
        # public endpoint surface and its semantics and policies, the
        # commissioned-product wrapper and the runtime core.
        knx_add_test(test_product_model SOURCES test/unit/test_product_model.cpp LABELS unit)
        knx_add_test(test_product_compiler SOURCES test/unit/test_product_compiler.cpp LABELS unit)
        knx_add_test(test_product_export SOURCES test/unit/test_product_export.cpp LABELS unit)
        knx_add_test(test_product_endpoint_public_surface SOURCES test/unit/test_product_endpoint_public_surface.cpp
            LABELS sdk-public-surface unit)
        knx_add_test(test_product_endpoint_semantics SOURCES test/unit/test_product_endpoint_semantics.cpp LABELS unit)
        knx_add_test(test_product_endpoint_policies SOURCES test/unit/test_product_endpoint_policies.cpp LABELS unit)
        knx_add_test(test_product_commissioned_surface SOURCES test/unit/test_product_commissioned_surface.cpp
            LABELS sdk-public-surface unit)
        knx_add_test(test_product_runtime_core SOURCES test/unit/test_product_runtime_core.cpp LABELS unit)

        knx_add_test(test_logging SOURCES test/unit/test_logging.cpp LABELS unit)
        knx_add_test(test_datalink_edge_cases SOURCES test/unit/test_datalink_edge_cases.cpp LABELS unit)
        knx_add_test(test_tp1_datalink_frame_pool SOURCES test/unit/test_tp1_datalink_frame_pool.cpp LABELS tp1 unit)
        knx_add_test(test_tp1_mac_controller SOURCES test/unit/test_tp1_mac_controller.cpp LABELS tp1 unit)
        knx_add_test(test_bitbang_driver_timer_isr SOURCES test/unit/test_bitbang_driver_timer_isr.cpp LABELS tp1 unit)

        # Virtual TP1 bus and virtual time.
        knx_add_test(test_virtual_time_engine SOURCES test/unit/test_virtual_time_engine.cpp
            LABELS ${TP1_VIRTUAL_UNIT})
        knx_add_test(test_virtual_test_clock SOURCES test/unit/test_virtual_test_clock.cpp
            LABELS ${TP1_VIRTUAL_UNIT})
        knx_add_test(test_virtual_platform SOURCES test/unit/test_virtual_platform.cpp
            LABELS ${TP1_VIRTUAL_UNIT} TIMEOUT 20)
        knx_add_test(test_virtual_tp1_test_runtime SOURCES test/unit/test_virtual_tp1_test_runtime.cpp
            LABELS ${TP1_VIRTUAL_UNIT} TIMEOUT 20)
        knx_add_test(test_timer_gpio_hal_virtual SOURCES test/unit/test_timer_gpio_hal_virtual.cpp
            LABELS ${TP1_VIRTUAL_UNIT})
        knx_add_test(test_tp1_bitbang_virtual_bus SOURCES test/unit/test_tp1_bitbang_virtual_bus.cpp
            LABELS ${TP1_VIRTUAL_UNIT})
        knx_add_test(test_tp1_bitbang_virtual_faults SOURCES test/unit/test_tp1_bitbang_virtual_faults.cpp
            LABELS ${TP1_VIRTUAL_UNIT})
        knx_add_test(test_tp1_virtual_trace_replay SOURCES test/unit/test_tp1_virtual_trace_replay.cpp
            LABELS ${TP1_VIRTUAL_UNIT} TIMEOUT 20)
        knx_add_test(test_tp1_datalink_virtual_timing SOURCES test/unit/test_tp1_datalink_virtual_timing.cpp
            LABELS ${TP1_VIRTUAL_UNIT} TIMEOUT 20)
        knx_add_test(test_tp1_datalink_virtual_bus_conformance SOURCES test/unit/test_tp1_datalink_virtual_bus_conformance.cpp
            LABELS ${TP1_VIRTUAL_UNIT} TIMEOUT 30)

        knx_add_test(test_bitbang_mac_physical SOURCES test/unit/test_bitbang_mac_physical.cpp LABELS tp1 unit)

        # Shell tests of the TP1 CI gate scripts.
        foreach(script
                test_tp1_ack_diagnostics_adapter_mapping_check
                test_tp1_ctest_regression
                test_tp1_benchmark_gate)
            file(CHMOD ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/${script}.sh
                PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
        endforeach()
        knx_add_test(test_tp1_ack_diagnostics_adapter_mapping_check_script
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ack_diagnostics_adapter_mapping_check.sh
            LABELS tp1 script)
        knx_add_test(test_tp1_ctest_regression_script
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ctest_regression.sh
            LABELS tp1 script)
        knx_add_test(test_tp1_benchmark_gate_script
            COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_benchmark_gate.sh
            LABELS tp1 script benchmark)

        knx_add_test(test_ets_roundtrip SOURCES test/integration/test_ets_roundtrip.cpp LABELS integration)
        knx_add_test(test_knxprog_importer SOURCES test/integration/test_knxprog_importer.cpp LABELS integration)
        knx_add_test(test_knx_stack_e2e SOURCES test/integration/test_knx_stack_e2e.cpp LABELS integration)
        knx_add_test(test_transport_application SOURCES test/integration/test_transport_application.cpp
            LABELS integration RUN_SERIAL)
        knx_add_test(test_transport_connection SOURCES test/unit/test_transport_connection.cpp LABELS unit TIMEOUT 20)
        knx_add_test(test_connection_state SOURCES test/unit/test_connection_state.cpp LABELS unit)
        knx_add_test(test_connection_table SOURCES test/unit/test_connection_table.cpp LABELS unit)

        # Outside the KNX_SECURE block on purpose: the Access Policies are
        # compiled into every build, and a build without Data Secure is
        # precisely the one where nothing can ever satisfy them — so it is
        # worth knowing they still deny.
        knx_add_test(test_security_access_policy SOURCES test/unit/test_security_access_policy.cpp LABELS unit secure)

        if(KNX_SECURE_ENABLED)
            knx_add_test(test_secure_application_layer SOURCES test/unit/test_secure_application_layer.cpp
                LABELS unit secure)
            knx_add_test(test_netip_secure_wrapper_vectors SOURCES test/unit/test_netip_secure_wrapper_vectors.cpp
                LABELS unit WORKING_DIRECTORY ${SRC_DIR})
            # Secure Routing multicast timer state machine (03_08_09 §2.2.2.3)
            knx_add_test(test_netip_mc_timer_sync SOURCES test/unit/test_netip_mc_timer_sync.cpp)
            # Secure Routing context: nonces, replay, TIMER_NOTIFY, persistence
            knx_add_test(test_netip_secure_routing SOURCES test/unit/test_netip_secure_routing.cpp)
            knx_add_test(test_key_derivation SOURCES test/unit/test_key_derivation.cpp LABELS unit)
        else()
            message(STATUS "KNX Secure disabled: skipping secure unit tests (test_secure_application_layer, test_key_derivation)")
        endif()

        # UDP loopback, and the configuration hooks
        knx_add_test(test_ip_physical SOURCES test/unit/test_ip_physical.cpp LABELS unit)
        knx_add_test(test_ip_physical_config SOURCES test/unit/test_ip_physical_config.cpp LABELS unit)
        knx_add_test(test_cemi SOURCES test/unit/test_cemi.cpp LABELS unit)
        knx_add_test(test_ets_config_loader SOURCES test/unit/test_ets_config_loader.cpp LABELS unit)
        knx_add_test(test_ets_format_validation SOURCES test/unit/test_ets_format_validation.cpp LABELS unit)

        # KNXnet/IP
        knx_add_test(test_netip_tunneling_server_endpoint SOURCES test/unit/test_netip_tunneling_server_endpoint.cpp)
        knx_add_test(test_tp1_ip_interface_bridge SOURCES test/unit/test_tp1_ip_interface_bridge.cpp)
        knx_add_test(test_netip_header_codec SOURCES test/unit/test_netip_header_codec.cpp LABELS unit)
        knx_add_test(test_netip_control_packet_codec SOURCES test/unit/test_netip_control_packet_codec.cpp)
        knx_add_test(test_netip_gateway_discovery_codec SOURCES test/unit/test_netip_gateway_discovery_codec.cpp)
        knx_add_test(test_netip_routing_flow_control SOURCES test/unit/test_netip_routing_flow_control.cpp LABELS unit)
        knx_add_test(test_netip_backbone_key SOURCES test/unit/test_netip_backbone_key.cpp LABELS unit)
        knx_add_test(test_netip_core_discovery SOURCES test/unit/test_netip_core_discovery.cpp LABELS unit)
        knx_add_test(test_netip_system_broadcast SOURCES test/unit/test_netip_system_broadcast.cpp LABELS unit)
        knx_add_test(test_netip_udp_datagram_channel SOURCES test/unit/test_netip_udp_datagram_channel.cpp LABELS unit)
        knx_add_test(test_netip_routing SOURCES test/unit/test_netip_routing.cpp LABELS unit)
        # Multicast join / rejoin
        knx_add_test(test_netip_routing_membership SOURCES test/unit/test_netip_routing_membership.cpp LABELS unit)
        # Transport security wrapper wiring
        knx_add_test(test_netip_security_plumbing SOURCES test/unit/test_netip_security_plumbing.cpp LABELS unit)

        if(KNX_SECURE_ENABLED)
            # TIMER_NOTIFY vector from Wireshark/KNXUltimate
            knx_add_test(test_netip_secure_timernotify_vector SOURCES test/unit/test_netip_secure_timernotify_vector.cpp
                LABELS unit WORKING_DIRECTORY ${SRC_DIR})
        endif()

        # Smoke test: TP1 <-> IP interface bridge path
        knx_add_test(test_tp1_ip_interface SOURCES test/integration/test_tp1_ip_interface.cpp LABELS integration)
        knx_add_test(test_netip_routing_multicast SOURCES test/integration/test_netip_routing_multicast.cpp
            LABELS integration)

        if(KNX_SECURE_ENABLED)
            # KNX/IP Secure routing multicast (SecureWrapper 0x0950)
            knx_add_test(test_netip_routing_multicast_secure SOURCES test/integration/test_netip_routing_multicast_secure.cpp
                LABELS integration)
        endif()

        # Tp1DataLinkLayer over the KNXnet/IP routing physical adapter
        knx_add_test(test_ip_routing_physical SOURCES test/integration/test_ip_routing_physical.cpp LABELS tp1 integration)
        # TP1 -> IP routing forwarding via coupler
        knx_add_test(test_tp1_to_ip_routing_coupler SOURCES test/integration/test_tp1_to_ip_routing_coupler.cpp
            LABELS tp1 integration)
        knx_add_test(test_coupler_stack_port SOURCES test/integration/test_coupler_stack_port.cpp LABELS tp1 integration)
        knx_add_test(test_two_port_coupler SOURCES test/integration/test_two_port_coupler.cpp LABELS tp1 integration)

        # The KNXnet/IP client role (tunnelling, gateway discovery, device
        # management, IP Secure sessions) exists only with tunnelling on.
        if(KNX_FEATURE_TUNNELING)
            knx_add_test(test_netip_tunneling SOURCES test/unit/test_netip_tunneling.cpp LABELS unit)
            knx_add_test(test_netip_tunneling_session_client SOURCES test/unit/test_netip_tunneling_session_client.cpp LABELS unit)
            knx_add_test(test_netip_secure_session_bootstrap SOURCES test/unit/test_netip_secure_session_bootstrap.cpp
                LABELS unit WORKING_DIRECTORY ${SRC_DIR})
            knx_add_test(test_netip_device_management_client SOURCES test/unit/test_netip_device_management_client.cpp)
            knx_add_test(test_netip_device_management_session SOURCES test/unit/test_netip_device_management_session.cpp
                NO_UNITY WORKING_DIRECTORY ${SRC_DIR})
            knx_add_test(test_netip_device_management_procedures SOURCES test/unit/test_netip_device_management_procedures.cpp
                NO_UNITY WORKING_DIRECTORY ${SRC_DIR})
            knx_add_test(test_netip_device_management_commissioner SOURCES test/unit/test_netip_device_management_commissioner.cpp
                NO_UNITY WORKING_DIRECTORY ${SRC_DIR})
            knx_add_test(test_netip_tcp_frame_channel SOURCES test/unit/test_netip_tcp_frame_channel.cpp LABELS unit)
            # Adapter wiring with Tp1DataLinkLayer
            knx_add_test(test_ip_tunneling_adapter SOURCES test/integration/test_ip_tunneling_adapter.cpp
                LABELS integration Long)
            knx_add_test(test_knxnetip_discovery SOURCES test/unit/test_knxnetip_discovery.cpp LIBS pthread)
            knx_add_test(test_netip_gateway_discovery_client SOURCES test/unit/test_netip_gateway_discovery_client.cpp
                LIBS pthread LABELS unit)
            knx_add_test(test_knxnetip_integration SOURCES test/integration/test_knxnetip_integration.cpp LIBS pthread LABELS integration)
            if(KNX_SECURE_ENABLED)
                # KNXUltimate-generated session vectors
                knx_add_test(test_netip_secure_session_vectors SOURCES test/unit/test_netip_secure_session_vectors.cpp
                    NO_UNITY LABELS unit WORKING_DIRECTORY ${SRC_DIR})
                # Scripted TCP handshake with vectors
                knx_add_test(test_netip_secure_tunneling_handshake SOURCES test/unit/test_netip_secure_tunneling_handshake.cpp
                    NO_UNITY LABELS unit WORKING_DIRECTORY ${SRC_DIR})
                # Tp1DataLinkLayer over the KNXnet/IP Secure tunnelling physical adapter
                knx_add_test(test_ip_secure_tunneling_physical SOURCES test/integration/test_ip_secure_tunneling_physical.cpp
                    LABELS integration WORKING_DIRECTORY ${SRC_DIR})
            endif()
        endif()

        # Interop helpers: KNstaX tunnelling and routing peers (used by the
        # Python interop harness)
        if(KNX_FEATURE_TUNNELING)
            add_executable(knstax_tunnel_peer
                interop/knstax_peer/knstax_tunnel_peer.cpp
            )
            target_link_libraries(knstax_tunnel_peer PRIVATE
                knx_core
            )
        endif()
        add_executable(knstax_routing_peer
            interop/knstax_peer/knstax_routing_peer.cpp
        )
        target_link_libraries(knstax_routing_peer PRIVATE
            knx_core
        )

        option(ENABLE_INTEROP_TESTS "Build and register the interop Python harness tests" OFF)
        if(ENABLE_INTEROP_TESTS AND NOT KNX_FEATURE_TUNNELING)
            message(FATAL_ERROR "ENABLE_INTEROP_TESTS needs KNX_FEATURE_TUNNELING: the harness drives knstax_tunnel_peer")
        endif()
        if(ENABLE_INTEROP_TESTS)
            set(INTEROP_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/interop/bin)
            file(MAKE_DIRECTORY ${INTEROP_BIN_DIR})

            # Place interop peer binaries in a stable location so tests can be
            # run reproducibly by CI (CTest will export env vars pointing here).
            set_target_properties(knstax_tunnel_peer PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${INTEROP_BIN_DIR})
            set_target_properties(knstax_routing_peer PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${INTEROP_BIN_DIR})

            # Detect whether pytest is available in the selected Python interpreter.
            execute_process(COMMAND ${Python3_EXECUTABLE} -m pytest --version
                            RESULT_VARIABLE PYTEST_EXIT_CODE
                            OUTPUT_QUIET ERROR_QUIET)
            if(PYTEST_EXIT_CODE EQUAL 0)
                # The interop harness may take longer than unit tests; allow up
                # to 15 minutes.
                knx_add_test(interop_pytest
                    COMMAND ${Python3_EXECUTABLE} -m pytest -q
                        ${CMAKE_CURRENT_SOURCE_DIR}/interop/python/tests
                        ${CMAKE_CURRENT_SOURCE_DIR}/tools/test_knxprod2profile_param_test.py
                    ENVIRONMENT
                        KNSTAX_TUNNEL_PEER_BIN=${INTEROP_BIN_DIR}/knstax_tunnel_peer
                        KNSTAX_ROUTING_PEER_BIN=${INTEROP_BIN_DIR}/knstax_routing_peer
                    LABELS Interop Integration Long
                    TIMEOUT 900
                    WORKING_DIRECTORY ${SRC_DIR})
            else()
                message(WARNING "Python interpreter found (${Python3_EXECUTABLE}) but pytest not available; skipping interop_pytest registration.")
            endif()
        endif()

        knx_add_test(test_ip_tp1_routing SOURCES test/integration/test_ip_tp1_routing.cpp LABELS tp1 integration)
        # InterfaceObjectManager persistence (host FilePersistence)
        knx_add_test(test_interface_object_manager_persistence
            SOURCES test/integration/test_interface_object_manager_persistence.cpp)

        # Transport, application layer and interface objects
        knx_add_test(test_transport_retransmission SOURCES test/unit/test_transport_retransmission.cpp)
        knx_add_test(test_device_descriptor SOURCES test/unit/test_device_descriptor.cpp)
        knx_add_test(test_property_services SOURCES test/unit/test_property_services.cpp)
        knx_add_test(test_property_ext_services SOURCES test/unit/test_property_ext_services.cpp)
        knx_add_test(test_interface_object_manager SOURCES test/unit/test_interface_object_manager.cpp)
        knx_add_test(test_interface_object_manager_instances SOURCES test/unit/test_interface_object_manager_instances.cpp)
        knx_add_test(test_memory_service SOURCES test/unit/test_memory_service.cpp)
        knx_add_test(test_authorization_service SOURCES test/unit/test_authorization_service.cpp)
        knx_add_test(test_restart_service SOURCES test/unit/test_restart_service.cpp)
        knx_add_test(test_application_layer SOURCES test/unit/test_application_layer.cpp)
        knx_add_test(test_application_integration SOURCES test/integration/test_application_integration.cpp)
        knx_add_test(test_application_virtual_lifecycle SOURCES test/integration/test_application_virtual_lifecycle.cpp
            LABELS tp1 integration virtual tp1-virtual)
        knx_add_test(test_device_object SOURCES test/unit/test_device_object.cpp)
        knx_add_test(test_inplace_function SOURCES test/unit/test_inplace_function.cpp LABELS unit)
        knx_add_test(test_fixed_vector SOURCES test/unit/test_fixed_vector.cpp LABELS unit)
        knx_add_test(test_application_program_object SOURCES test/unit/test_application_program_object.cpp)
        knx_add_test(test_address_association_tables SOURCES test/unit/test_address_association_tables.cpp)
        knx_add_test(test_group_object_table SOURCES test/unit/test_group_object_table.cpp)
        knx_add_test(test_security_interface_object SOURCES test/unit/test_security_interface_object.cpp)
        knx_add_test(test_filter_table SOURCES test/unit/test_filter_table.cpp)

        # Datapoint types
        knx_add_test(test_dpt2_3_4 SOURCES test/unit/test_dpt2_3_4.cpp)
        knx_add_test(test_dpt6_7_8 SOURCES test/unit/test_dpt6_7_8.cpp)
        knx_add_test(test_dpt10_11_19 SOURCES test/unit/test_dpt10_11_19.cpp)
        knx_add_test(test_dpt12_13_14 SOURCES test/unit/test_dpt12_13_14.cpp)
        knx_add_test(test_dpt16_17 SOURCES test/unit/test_dpt16_17.cpp)
        knx_add_test(test_dpt_extended SOURCES test/unit/test_dpt_extended.cpp)

        knx_add_test(test_error_injection SOURCES test/unit/test_error_injection.cpp)
        knx_add_test(test_protocol_conformance SOURCES test/integration/test_protocol_conformance.cpp)
        knx_add_test(test_netip_secured_service_families SOURCES test/unit/test_netip_secured_service_families.cpp
            LABELS unit TIMEOUT 20)
        # Event-driven receive plumbing
        knx_add_test(test_netip_socket_wait SOURCES test/unit/test_netip_socket_wait.cpp LABELS unit TIMEOUT 20)
        # BAU on-wire golden tests
        knx_add_test(test_bau_onwire SOURCES test/integration/test_bau_onwire.cpp LABELS integration)
        # Connection-oriented ETS session
        knx_add_test(test_ets_connected_session SOURCES test/integration/test_ets_connected_session.cpp TIMEOUT 30 LABELS integration)
        # Started product runtime
        knx_add_test(test_product_runtime SOURCES test/integration/test_product_runtime.cpp LABELS integration)
        # Conformance evidence, commissioned path
        knx_add_test(test_conformance_evidence_commissioned
            SOURCES test/conformance/test_conformance_evidence_commissioned.cpp
            LABELS conformance sdk-public-surface)
        # ETS parameters -> transmit policy
        knx_add_test(test_transmit_policy_ets_parameters SOURCES test/integration/test_transmit_policy_ets_parameters.cpp
            LABELS integration)

        if(KNX_PLATFORM STREQUAL "linux")
            knx_add_test(test_linux_platform SOURCES test/unit/test_linux_platform.cpp LABELS unit)
        endif()
    endif()  # BUILD_TESTS: end of the test-only section

    if(KNX_BUILD_EXAMPLES)


        add_executable(tp1_thermostat
            examples/tp1_thermostat/main.cpp
        )
        target_link_libraries(tp1_thermostat PRIVATE
            knx_core
        )

        # A coupler built through the ordinary commissioned-product path: the
        # only difference from an end device is CouplerOptions.
        add_executable(tp1_line_coupler
            examples/tp1_line_coupler/main.cpp
        )
        target_link_libraries(tp1_line_coupler PRIVATE
            knx_core
        )




        add_executable(knxnetip_routing_sniffer
            examples/knxnetip_routing_sniffer.cpp
        )
        target_link_libraries(knxnetip_routing_sniffer PRIVATE
            knx_core
        )


        add_executable(ip_routing_device
            examples/ip_routing_device/main.cpp
        )
        target_link_libraries(ip_routing_device PRIVATE
            knx_core
        )

        # The tunnelling examples are KNXnet/IP clients.
        set(KNX_TUNNELING_EXAMPLES)
        if(KNX_FEATURE_TUNNELING)
            add_executable(ip_device
                examples/ip_device/main.cpp
            )
            target_link_libraries(ip_device PRIVATE
                knx_core
            )

            add_executable(tp1_ip_interface
                examples/tp1_ip_interface/main.cpp
            )
            target_link_libraries(tp1_ip_interface PRIVATE
                knx_core
            )
            set(KNX_TUNNELING_EXAMPLES ip_device tp1_ip_interface)
        endif()

        if(BUILD_TESTS)
            knx_add_test(test_knxnetip_routing_sniffer_e2e
                SOURCES test/integration/test_knxnetip_routing_sniffer_e2e.cpp
                ARGS $<TARGET_FILE:knxnetip_routing_sniffer>)
        endif()

        add_custom_target(knx_sdk_example_contracts
            DEPENDS
                tp1_thermostat
                tp1_line_coupler
                knxnetip_routing_sniffer
                ip_routing_device
                ${KNX_TUNNELING_EXAMPLES}
        )

        # Build-time .knxprod generation, demonstrating the single-source-of-truth
        # DX: product.hpp → .knxprod.xml with no hand-written ETS file. Driven
        # from the ESP-IDF switch example's definition, which proves the point
        # that matters — the same header compiles for target firmware and for
        # the host exporter.
        knx_commissioned_product(tp1_switch_ets
            DEFINITION  ${CMAKE_CURRENT_SOURCE_DIR}/examples/esp_idf_switch/main/product.hpp
            SYMBOL      tp1_switch::kProduct
        )

        if(BUILD_TESTS)
            add_custom_target(knx_sdk_release_gate
                COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_CURRENT_BINARY_DIR} --output-on-failure -L sdk-public-surface
                USES_TERMINAL
                COMMENT "Running KNstaX SDK release gate"
            )
            # The gate runs labels, so it depends on every executable carrying
            # them; listing them by hand once left one out, and the gate failed
            # with "Unable to find executable" on a clean tree.
            knx_tests_with_label(KNX_SDK_PUBLIC_SURFACE_TEST_TARGETS sdk-public-surface)
            add_dependencies(knx_sdk_release_gate
                knx_sdk_public_header_contracts
                knx_sdk_example_contracts
                ${KNX_SDK_PUBLIC_SURFACE_TEST_TARGETS}
            )
        endif()
    endif()

    if(KNX_FEATURE_TP1)
        add_executable(benchmark_tp1
            benchmark/benchmark_tp1.cpp
        )
        target_link_libraries(benchmark_tp1 PRIVATE
            knx_core
        )
    endif()

    # ========================================================================
    # Summary
    # ========================================================================

    message(STATUS "")
    message(STATUS "KNX Stack Configuration:")
    message(STATUS "  Platform:          ${KNX_PLATFORM}")
    message(STATUS "  Build Type:        ${CMAKE_BUILD_TYPE}")
    message(STATUS "  C++ Standard:      ${CMAKE_CXX_STANDARD}")
    message(STATUS "")
    message(STATUS "Build Targets:")
    message(STATUS "  Library:           knx_core")
    message(STATUS "  Tests:             test_types, test_dpt, test_frame_encoding, test_tp1_integration")
    message(STATUS "")
    message(STATUS "To build: cmake --build .")
    message(STATUS "To test:  ctest --output-on-failure")
    message(STATUS "")

endif()

if(NOT DEFINED IDF_PATH AND NOT COMMAND idf_component_register)
    # Install targets and package configuration for standalone builds
    include(CMakePackageConfigHelpers)

    set(INSTALL_CONFIGDIR lib/cmake/KNstaX)

    install(TARGETS knx_core
        EXPORT KNstaXTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
    )

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ DESTINATION include)

    install(EXPORT KNstaXTargets
        FILE KNstaXTargets.cmake
        NAMESPACE KNX::
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/KNstaXConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/KNstaXConfig.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/KNstaXConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/KNstaXConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/KNstaXConfigVersion.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )
    # CPack packaging configuration (standalone builds)
    if(NOT DEFINED IDF_PATH AND NOT COMMAND idf_component_register)
        set(CPACK_GENERATOR "TGZ;ZIP")
        set(CPACK_PACKAGE_NAME "knstax")
        set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
        set(CPACK_PACKAGE_CONTACT "maintainers@knstax.local")
        set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
        set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_NAME}")
        include(CPack)
    endif()
endif()
