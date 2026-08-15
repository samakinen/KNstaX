# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 Sami Mäkinen

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/knx_compiler.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/knx_sources.cmake")

option(KNX_WARNINGS_AS_ERRORS "Treat KNstaX compiler warnings as errors" OFF)

# Single source of truth for the stack version.  Keep in sync with
# idf_component.yml and include/knx/knx.hpp.
set(KNX_VERSION_STRING "0.2.0-alpha")

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
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_TP1} ${KNX_SOURCES_TPUART})
    endif()
    if(KNX_FEATURE_NETIP)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_NETIP})
    endif()
    if(KNX_FEATURE_TP1 AND KNX_FEATURE_NETIP)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_TP1_NETIP_BRIDGE})
    endif()
    if(CONFIG_KNX_VIRTUAL_BUS)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_VIRTUAL})
    endif()

    # KNX Secure on ESP-IDF.  The crypto primitives call into mbedTLS from the
    # IDF, but they are still translation units of their own: without them the
    # Data Secure session (CCM protect/unprotect) has no definition and the
    # stack links only because every `#if KNX_SECURE_ENABLED` guard is off.
    # The IP Secure transport additionally requires the KNXnet/IP group.
    if(KNX_FEATURE_SECURE)
        list(APPEND KNX_COMPONENT_SOURCES ${KNX_SOURCES_SECURE_DATA})
        if(KNX_FEATURE_NETIP)
            list(APPEND KNX_COMPONENT_SOURCES
                ${KNX_SOURCES_SECURE_IP_CRYPTO}
                ${KNX_SOURCES_SECURE_NETIP})
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
            esp_driver_uart
            esp_driver_spi
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
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        KNX_FEATURE_NETIP=${KNX_FEATURE_NETIP}
        KNX_FEATURE_TP1=${KNX_FEATURE_TP1}
        KNX_SECURE_ENABLED=${KNX_FEATURE_SECURE}
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        KNX_STACK_VERSION="${KNX_VERSION_STRING}"
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
    project(KNstaX VERSION 1.0.0 LANGUAGES C CXX)
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

    # Production build safety option
    option(KNX_PRODUCTION_BUILD "Production build mode (project-level flag for stricter settings)" OFF)
    
    if(KNX_PRODUCTION_BUILD)
        message(STATUS "KNX Production Build: ENABLED (stubs will cause build errors)")
        add_compile_definitions(KNX_PRODUCTION_BUILD=1)
    else()
        message(STATUS "KNX Production Build: DISABLED (stubs allowed for testing)")
    endif()

    # Optional feature toggles
    # Note: This repository intentionally avoids shipping stub implementations in `src/`.

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
    # ------------------------------------------------------------------
    option(KNX_FEATURE_TP1 "Build the TP1 medium" ON)
    option(KNX_FEATURE_NETIP "Build the KNXnet/IP medium" ON)
    option(KNX_FEATURE_VIRTUAL_BUS "Build the virtual TP1 bus simulator" ON)
    option(KNX_FEATURE_ETS_TOOLING "Build the ETS import/migration tooling" ON)

    set(COMMON_SOURCES ${KNX_SOURCES_CORE})

    if(KNX_FEATURE_TP1)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_TP1})
    endif()
    if(KNX_FEATURE_NETIP)
        list(APPEND COMMON_SOURCES ${KNX_SOURCES_NETIP})
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
            list(APPEND COMMON_SOURCES ${KNX_SOURCES_SECURE_NETIP})
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
        KNX_HAS_ZLIB=$<IF:$<BOOL:${KNX_HAS_ZLIB}>,1,0>
        KNX_FEATURE_NETIP=$<IF:$<BOOL:${KNX_FEATURE_NETIP}>,1,0>
        KNX_FEATURE_TP1=$<IF:$<BOOL:${KNX_FEATURE_TP1}>,1,0>
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

    # Test executable: test_types
    add_executable(test_types
        test/unit/test_types.cpp
    )
    target_link_libraries(test_types PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_types COMMAND test_types)

    # Test executable: test_dpt
    add_executable(test_dpt
        test/unit/test_dpt.cpp
    )
    target_link_libraries(test_dpt PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt COMMAND test_dpt)

    # Test executable: test_manchester_codec
    add_executable(test_manchester_codec
        test/unit/test_manchester_codec.cpp
    )
    target_link_libraries(test_manchester_codec PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_manchester_codec COMMAND test_manchester_codec)

    # Test executable: test_telegram_rate_limiter
    add_executable(test_telegram_rate_limiter
        test/unit/test_telegram_rate_limiter.cpp
    )
    target_link_libraries(test_telegram_rate_limiter PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_telegram_rate_limiter COMMAND test_telegram_rate_limiter)

    # Test executable: test_group_object_transmit_policy
    add_executable(test_group_object_transmit_policy
        test/unit/test_group_object_transmit_policy.cpp
    )
    target_link_libraries(test_group_object_transmit_policy PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_group_object_transmit_policy COMMAND test_group_object_transmit_policy)

    # Test executable: test_group_object_flags
    add_executable(test_group_object_flags
        test/unit/test_group_object_flags.cpp
    )
    target_link_libraries(test_group_object_flags PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_group_object_flags COMMAND test_group_object_flags)
    set_tests_properties(test_group_object_flags PROPERTIES LABELS "unit")

    # Test executable: test_function_property_services
    add_executable(test_function_property_services
        test/unit/test_function_property_services.cpp
    )
    target_link_libraries(test_function_property_services PRIVATE knx_core unity)
    add_test(NAME test_function_property_services COMMAND test_function_property_services)
    set_tests_properties(test_function_property_services PROPERTIES LABELS "unit")

    # Test executable: test_network_parameter_service
    add_executable(test_network_parameter_service
        test/unit/test_network_parameter_service.cpp
    )
    target_link_libraries(test_network_parameter_service PRIVATE knx_core unity)
    add_test(NAME test_network_parameter_service COMMAND test_network_parameter_service)
    set_tests_properties(test_network_parameter_service PROPERTIES LABELS "unit")

    # Test executable: test_endpoint_semantics
    add_executable(test_endpoint_semantics
        test/unit/test_endpoint_semantics.cpp
    )
    target_link_libraries(test_endpoint_semantics PRIVATE knx_core unity)
    add_test(NAME test_endpoint_semantics COMMAND test_endpoint_semantics)
    set_tests_properties(test_endpoint_semantics PROPERTIES LABELS "unit")

    # Test executable: test_routing_table_control
    add_executable(test_routing_table_control
        test/unit/test_routing_table_control.cpp
    )
    target_link_libraries(test_routing_table_control PRIVATE knx_core unity)
    add_test(NAME test_routing_table_control COMMAND test_routing_table_control)
    set_tests_properties(test_routing_table_control PROPERTIES LABELS "unit")

    # Test executable: test_coupler_routing
    add_executable(test_coupler_routing
        test/unit/test_coupler_routing.cpp
    )
    target_link_libraries(test_coupler_routing PRIVATE knx_core unity)
    add_test(NAME test_coupler_routing COMMAND test_coupler_routing)
    set_tests_properties(test_coupler_routing PROPERTIES LABELS "unit")

    # Test executable: test_bau_router_role
    add_executable(test_bau_router_role
        test/integration/test_bau_router_role.cpp
    )
    target_link_libraries(test_bau_router_role PRIVATE knx_core unity)
    add_test(NAME test_bau_router_role COMMAND test_bau_router_role)
    set_tests_properties(test_bau_router_role PROPERTIES LABELS "integration")

    # Test executable: test_persistence_schema_version
    add_executable(test_persistence_schema_version
        test/integration/test_persistence_schema_version.cpp
    )
    target_link_libraries(test_persistence_schema_version PRIVATE knx_core unity)
    add_test(NAME test_persistence_schema_version COMMAND test_persistence_schema_version)
    set_tests_properties(test_persistence_schema_version PROPERTIES
        LABELS "integration"
        RUN_SERIAL TRUE
    )

    # Test executable: test_link_monitor
    add_executable(test_link_monitor
        test/unit/test_link_monitor.cpp
    )
    target_link_libraries(test_link_monitor PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_link_monitor COMMAND test_link_monitor)
    set_tests_properties(test_link_monitor PROPERTIES LABELS "physical;unit")

    # Test executable: test_tp1_frame_codec
    add_executable(test_tp1_frame_codec
        test/unit/test_tp1_frame_codec.cpp
    )
    target_link_libraries(test_tp1_frame_codec PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_frame_codec COMMAND test_tp1_frame_codec)
    set_tests_properties(test_tp1_frame_codec PROPERTIES LABELS "tp1;unit")

    # Test executable: test_tp1_frame_golden
    add_executable(test_tp1_frame_golden
        test/unit/test_tp1_frame_golden.cpp
    )
    target_link_libraries(test_tp1_frame_golden PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_frame_golden COMMAND test_tp1_frame_golden)
    set_tests_properties(test_tp1_frame_golden PROPERTIES LABELS "tp1;unit")

    # Test executable: test_frame_encoding
    add_executable(test_frame_encoding
        test/unit/test_frame_encoding.cpp
    )
    target_link_libraries(test_frame_encoding PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_frame_encoding COMMAND test_frame_encoding)

    # Test: knxprod exporter validation (runs wrapper + basic XML checks)
    add_test(NAME test_knxprod_exporter COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/knxprod_exporter/test_validate_knxprod.py)
    # Test: knxprod XSD validation (runs wrapper + XSD validation via xmlschema)
    add_test(NAME test_knxprod_xsd_validation COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/knxprod_exporter/test_validate_knxprod_xsd.py)
    # Test: exporter parameter UX and load procedure (pure-python, no wrapper)
    add_test(NAME test_exporter_parameter_ux
             COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/knxprod_exporter/test_exporter_parameter_ux.py)
    set_tests_properties(test_exporter_parameter_ux PROPERTIES LABELS "script")
    # Serialize exporter tests and route generated files to the build tree.
    set_tests_properties(
        test_knxprod_exporter
        test_knxprod_xsd_validation
        PROPERTIES
            RUN_SERIAL TRUE
            ENVIRONMENT "KNXPROD_EXPORTER_OUT_DIR=${CMAKE_BINARY_DIR}/knxprod_exporter_artifacts"
    )
    
    # Test executable: test_frame_codec_errors
    add_executable(test_frame_codec_errors
        test/unit/test_frame_codec_errors.cpp
    )
    target_link_libraries(test_frame_codec_errors PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_frame_codec_errors COMMAND test_frame_codec_errors)

    # Test executable: test_decoder_robustness (decoder fuzz/robustness)
    add_executable(test_decoder_robustness
        test/unit/test_decoder_robustness.cpp
    )
    target_link_libraries(test_decoder_robustness PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_decoder_robustness COMMAND test_decoder_robustness)

    # Test executable: test_tp1_integration
    add_executable(test_tp1_integration
        test/integration/test_tp1_integration.cpp
    )
    target_link_libraries(test_tp1_integration PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_integration COMMAND test_tp1_integration)
    set_tests_properties(test_tp1_integration PROPERTIES LABELS "tp1;integration")

# PoC: generate product profile header from a .knxprod file
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated_product_profile.hpp
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/knxprod2profile.py ${CMAKE_SOURCE_DIR}/tools/examples/sample.knxprod.xml --output ${CMAKE_BINARY_DIR}/generated_product_profile.hpp
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/knxprod2profile.py ${CMAKE_SOURCE_DIR}/templates/profile_header.tpl ${CMAKE_SOURCE_DIR}/tools/examples/sample.knxprod.xml
)
add_custom_target(generate_product_profile DEPENDS ${CMAKE_BINARY_DIR}/generated_product_profile.hpp)

# Single-source-of-truth product export module.
# Provides knx_commissioned_product() which generates .knxprod.xml from a
# constexpr C++ product definition at build time.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/knx_product.cmake)

    # Test executable: test_result
    add_executable(test_result
        test/unit/test_result.cpp
    )
    target_link_libraries(test_result PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_result COMMAND test_result)

    # Test executable: test_config_validation
    add_executable(test_config_validation
        test/unit/test_config_validation.cpp
    )
    target_link_libraries(test_config_validation PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_config_validation COMMAND test_config_validation)

    # Test executable: test_exporter
    add_executable(test_exporter
        test/unit/test_exporter.cpp
    )
    target_link_libraries(test_exporter PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_exporter COMMAND test_exporter)

    # Test executable: test_product_model (M1 rewrite-lane definition/bindings core)
    add_executable(test_product_model
        test/unit/test_product_model.cpp
    )
    target_link_libraries(test_product_model PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_model COMMAND test_product_model)

    # Test executable: test_product_compiler (M1 rewrite-lane compiler core)
    add_executable(test_product_compiler
        test/unit/test_product_compiler.cpp
    )
    target_link_libraries(test_product_compiler PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_compiler COMMAND test_product_compiler)

    # Test executable: test_product_export (M1 rewrite-lane export bridge)
    add_executable(test_product_export
        test/unit/test_product_export.cpp
    )
    target_link_libraries(test_product_export PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_export COMMAND test_product_export)

    # Test executable: test_product_endpoint_public_surface (canonical public endpoint cutover)
    add_executable(test_product_endpoint_public_surface
        test/unit/test_product_endpoint_public_surface.cpp
    )
    target_link_libraries(test_product_endpoint_public_surface PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_endpoint_public_surface COMMAND test_product_endpoint_public_surface)
    set_property(TEST test_product_endpoint_public_surface APPEND PROPERTY LABELS "sdk-public-surface")

    # Test executable: test_product_endpoint_semantics (canonical endpoint semantics)
    add_executable(test_product_endpoint_semantics
        test/unit/test_product_endpoint_semantics.cpp
    )
    target_link_libraries(test_product_endpoint_semantics PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_endpoint_semantics COMMAND test_product_endpoint_semantics)

    # Test executable: test_product_endpoint_policies (canonical endpoint policies)
    add_executable(test_product_endpoint_policies
        test/unit/test_product_endpoint_policies.cpp
    )
    target_link_libraries(test_product_endpoint_policies PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_endpoint_policies COMMAND test_product_endpoint_policies)

    # Test executable: test_product_commissioned_surface (commissioned product public wrapper)
    add_executable(test_product_commissioned_surface
        test/unit/test_product_commissioned_surface.cpp
    )
    target_link_libraries(test_product_commissioned_surface PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_commissioned_surface COMMAND test_product_commissioned_surface)
    set_property(TEST test_product_commissioned_surface APPEND PROPERTY LABELS "sdk-public-surface")

    # Test executable: test_product_runtime_core (M1 rewrite-lane runtime core)
    add_executable(test_product_runtime_core
        test/unit/test_product_runtime_core.cpp
    )
    target_link_libraries(test_product_runtime_core PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_runtime_core COMMAND test_product_runtime_core)

    # Test executable: test_logging
    add_executable(test_logging
        test/unit/test_logging.cpp
    )
    target_link_libraries(test_logging PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_logging COMMAND test_logging)

    # Test executable: test_datalink_edge_cases
    add_executable(test_datalink_edge_cases
        test/unit/test_datalink_edge_cases.cpp
    )
    target_link_libraries(test_datalink_edge_cases PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_datalink_edge_cases COMMAND test_datalink_edge_cases)

    # Test executable: test_tp1_datalink_frame_pool
    add_executable(test_tp1_datalink_frame_pool
        test/unit/test_tp1_datalink_frame_pool.cpp
    )
    target_link_libraries(test_tp1_datalink_frame_pool PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_datalink_frame_pool COMMAND test_tp1_datalink_frame_pool)
    set_tests_properties(test_tp1_datalink_frame_pool PROPERTIES LABELS "tp1;unit")

    # Test executable: test_tp1_mac_controller
    add_executable(test_tp1_mac_controller
        test/unit/test_tp1_mac_controller.cpp
    )
    target_link_libraries(test_tp1_mac_controller PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_mac_controller COMMAND test_tp1_mac_controller)
    set_tests_properties(test_tp1_mac_controller PROPERTIES LABELS "tp1;unit")

    # Test executable: test_bitbang_driver_timer_isr
    add_executable(test_bitbang_driver_timer_isr
        test/unit/test_bitbang_driver_timer_isr.cpp
    )
    target_link_libraries(test_bitbang_driver_timer_isr PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_bitbang_driver_timer_isr COMMAND test_bitbang_driver_timer_isr)
    set_tests_properties(test_bitbang_driver_timer_isr PROPERTIES LABELS "tp1;unit")

    # Test executable: test_virtual_time_engine
    add_executable(test_virtual_time_engine
        test/unit/test_virtual_time_engine.cpp
    )
    target_link_libraries(test_virtual_time_engine PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_virtual_time_engine COMMAND test_virtual_time_engine)
    set_tests_properties(test_virtual_time_engine PROPERTIES LABELS "tp1;unit;virtual")

    # Test executable: test_virtual_test_clock
    add_executable(test_virtual_test_clock
        test/unit/test_virtual_test_clock.cpp
    )
    target_link_libraries(test_virtual_test_clock PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_virtual_test_clock COMMAND test_virtual_test_clock)
    set_tests_properties(test_virtual_test_clock PROPERTIES LABELS "tp1;unit;virtual;tp1-virtual")

    # Test executable: test_virtual_platform
    add_executable(test_virtual_platform
        test/unit/test_virtual_platform.cpp
    )
    target_link_libraries(test_virtual_platform PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_virtual_platform COMMAND test_virtual_platform)
    set_tests_properties(test_virtual_platform PROPERTIES
        LABELS "tp1;unit;virtual;tp1-virtual"
        TIMEOUT 20
    )
    set_property(TEST test_virtual_platform APPEND PROPERTY LABELS "sdk-flake-candidate")

    # Test executable: test_virtual_tp1_test_runtime
    add_executable(test_virtual_tp1_test_runtime
        test/unit/test_virtual_tp1_test_runtime.cpp
    )
    target_link_libraries(test_virtual_tp1_test_runtime PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_virtual_tp1_test_runtime COMMAND test_virtual_tp1_test_runtime)
    set_tests_properties(test_virtual_tp1_test_runtime PROPERTIES
        LABELS "tp1;unit;virtual;tp1-virtual"
        TIMEOUT 20
    )
    set_property(TEST test_virtual_tp1_test_runtime APPEND PROPERTY LABELS "sdk-flake-candidate")

    # Test executable: test_timer_gpio_hal_virtual
    add_executable(test_timer_gpio_hal_virtual
        test/unit/test_timer_gpio_hal_virtual.cpp
    )
    target_link_libraries(test_timer_gpio_hal_virtual PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_timer_gpio_hal_virtual COMMAND test_timer_gpio_hal_virtual)
    set_tests_properties(test_timer_gpio_hal_virtual PROPERTIES LABELS "tp1;unit;virtual")

    # Test executable: test_tp1_bitbang_virtual_bus
    add_executable(test_tp1_bitbang_virtual_bus
        test/unit/test_tp1_bitbang_virtual_bus.cpp
    )
    target_link_libraries(test_tp1_bitbang_virtual_bus PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_bitbang_virtual_bus COMMAND test_tp1_bitbang_virtual_bus)
    set_tests_properties(test_tp1_bitbang_virtual_bus PROPERTIES LABELS "tp1;unit;virtual")
    set_property(TEST test_tp1_bitbang_virtual_bus APPEND PROPERTY LABELS "sdk-flake-candidate")

    # Test executable: test_tp1_bitbang_virtual_faults
    add_executable(test_tp1_bitbang_virtual_faults
        test/unit/test_tp1_bitbang_virtual_faults.cpp
    )
    target_link_libraries(test_tp1_bitbang_virtual_faults PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_bitbang_virtual_faults COMMAND test_tp1_bitbang_virtual_faults)
    set_tests_properties(test_tp1_bitbang_virtual_faults PROPERTIES LABELS "tp1;unit;virtual")

    # Test executable: test_tp1_virtual_trace_replay
    add_executable(test_tp1_virtual_trace_replay
        test/unit/test_tp1_virtual_trace_replay.cpp
    )
    target_link_libraries(test_tp1_virtual_trace_replay PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_virtual_trace_replay COMMAND test_tp1_virtual_trace_replay)
    set_tests_properties(test_tp1_virtual_trace_replay PROPERTIES
        LABELS "tp1;unit;virtual;tp1-virtual"
        TIMEOUT 20
    )
    set_property(TEST test_tp1_virtual_trace_replay APPEND PROPERTY LABELS "sdk-flake-candidate")

    add_executable(test_tp1_datalink_virtual_timing
        test/unit/test_tp1_datalink_virtual_timing.cpp
    )
    target_link_libraries(test_tp1_datalink_virtual_timing PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_datalink_virtual_timing COMMAND test_tp1_datalink_virtual_timing)
    set_tests_properties(test_tp1_datalink_virtual_timing PROPERTIES
        LABELS "tp1;unit;virtual;tp1-virtual"
        TIMEOUT 20
    )
    set_property(TEST test_tp1_datalink_virtual_timing APPEND PROPERTY LABELS "sdk-flake-candidate")

    add_executable(test_tp1_datalink_virtual_bus_conformance
        test/unit/test_tp1_datalink_virtual_bus_conformance.cpp
    )
    target_link_libraries(test_tp1_datalink_virtual_bus_conformance PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_datalink_virtual_bus_conformance COMMAND test_tp1_datalink_virtual_bus_conformance)
    set_tests_properties(test_tp1_datalink_virtual_bus_conformance PROPERTIES
        LABELS "tp1;unit;virtual;tp1-virtual"
        TIMEOUT 30
    )

    set_property(TEST test_virtual_time_engine APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_virtual_test_clock APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_virtual_platform APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_virtual_tp1_test_runtime APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_timer_gpio_hal_virtual APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_tp1_bitbang_virtual_bus APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_tp1_bitbang_virtual_faults APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_tp1_datalink_virtual_timing APPEND PROPERTY LABELS "tp1-virtual")
    set_property(TEST test_tp1_datalink_virtual_bus_conformance APPEND PROPERTY LABELS "tp1-virtual")

    # Test executable: test_tpuart_medium_backend_adapter
    add_executable(test_tpuart_medium_backend_adapter
        test/unit/test_tpuart_medium_backend_adapter.cpp
    )
    target_link_libraries(test_tpuart_medium_backend_adapter PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tpuart_medium_backend_adapter COMMAND test_tpuart_medium_backend_adapter)
    set_tests_properties(test_tpuart_medium_backend_adapter PROPERTIES LABELS "tp1;unit")

    # Test executable: test_bitbang_mac_physical
    add_executable(test_bitbang_mac_physical
        test/unit/test_bitbang_mac_physical.cpp
    )
    target_link_libraries(test_bitbang_mac_physical PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_bitbang_mac_physical COMMAND test_bitbang_mac_physical)
    set_tests_properties(test_bitbang_mac_physical PROPERTIES LABELS "tp1;unit")

    # Test executable: test_tpuart_mac_physical
    add_executable(test_tpuart_mac_physical
        test/unit/test_tpuart_mac_physical.cpp
    )
    target_link_libraries(test_tpuart_mac_physical PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tpuart_mac_physical COMMAND test_tpuart_mac_physical)
    set_tests_properties(test_tpuart_mac_physical PROPERTIES LABELS "tp1;unit")

    # Test executable: test_physical_factory_tp1
    add_executable(test_physical_factory_tp1
        test/unit/test_physical_factory_tp1.cpp
    )
    target_link_libraries(test_physical_factory_tp1 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_physical_factory_tp1 COMMAND test_physical_factory_tp1)
    set_tests_properties(test_physical_factory_tp1 PROPERTIES LABELS "tp1;unit")

    file(CHMOD
        ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ack_diagnostics_adapter_mapping_check.sh
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
    file(CHMOD
        ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ctest_regression.sh
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
    file(CHMOD
        ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_benchmark_gate.sh
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
    add_test(
        NAME test_tp1_ack_diagnostics_adapter_mapping_check_script
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ack_diagnostics_adapter_mapping_check.sh
    )
    set_tests_properties(test_tp1_ack_diagnostics_adapter_mapping_check_script PROPERTIES LABELS "tp1;script")
    add_test(
        NAME test_tp1_ctest_regression_script
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_ctest_regression.sh
    )
    set_tests_properties(test_tp1_ctest_regression_script PROPERTIES LABELS "tp1;script")
    add_test(
        NAME test_tp1_benchmark_gate_script
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/scripts/test_tp1_benchmark_gate.sh
    )
    set_tests_properties(test_tp1_benchmark_gate_script PROPERTIES LABELS "tp1;script;benchmark")

    # Test executable: test_ets_roundtrip
    add_executable(test_ets_roundtrip
        test/integration/test_ets_roundtrip.cpp
    )
    target_link_libraries(test_ets_roundtrip PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ets_roundtrip COMMAND test_ets_roundtrip)

    # Test executable: test_knxprog_importer
    add_executable(test_knxprog_importer
        test/integration/test_knxprog_importer.cpp
    )
    target_link_libraries(test_knxprog_importer PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_knxprog_importer COMMAND test_knxprog_importer)

    # Test executable: test_knx_stack_e2e
    add_executable(test_knx_stack_e2e
        test/integration/test_knx_stack_e2e.cpp
    )
    target_link_libraries(test_knx_stack_e2e PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_knx_stack_e2e COMMAND test_knx_stack_e2e)

    # Test executable: test_transport_application
    add_executable(test_transport_application
        test/integration/test_transport_application.cpp
    )
    target_link_libraries(test_transport_application PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_transport_application COMMAND test_transport_application)
    set_tests_properties(test_transport_application PROPERTIES
        LABELS "integration"
        RUN_SERIAL TRUE
    )

    # Test executable: test_transport_connection
    add_executable(test_transport_connection
        test/unit/test_transport_connection.cpp
    )
    target_link_libraries(test_transport_connection PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_transport_connection COMMAND test_transport_connection)
    set_tests_properties(test_transport_connection PROPERTIES TIMEOUT 20)

    # Test executable: test_connection_state
    add_executable(test_connection_state
        test/unit/test_connection_state.cpp
    )
    target_link_libraries(test_connection_state PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_connection_state COMMAND test_connection_state)

    # Test executable: test_connection_table
    add_executable(test_connection_table
        test/unit/test_connection_table.cpp
    )
    target_link_libraries(test_connection_table PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_connection_table COMMAND test_connection_table)

    # Test executable: test_security_access_policy.  Outside the KNX_SECURE
    # block on purpose: the Access Policies are compiled into every build, and
    # a build without Data Secure is precisely the one where nothing can ever
    # satisfy them — so it is worth knowing they still deny.
    add_executable(test_security_access_policy
        test/unit/test_security_access_policy.cpp
    )
    target_link_libraries(test_security_access_policy PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_security_access_policy COMMAND test_security_access_policy)
    set_tests_properties(test_security_access_policy PROPERTIES LABELS "unit;secure")

    if(KNX_SECURE_ENABLED)
        # Test executable: test_secure_application_layer
        add_executable(test_secure_application_layer
            test/unit/test_secure_application_layer.cpp
        )
        target_link_libraries(test_secure_application_layer PRIVATE knx_core unity)
        add_test(NAME test_secure_application_layer COMMAND test_secure_application_layer)
        set_tests_properties(test_secure_application_layer PROPERTIES LABELS "unit;secure")

        # Test executable: test_secure_vectors
        add_executable(test_secure_vectors
            test/unit/test_secure_vectors.cpp
        )
        target_link_libraries(test_secure_vectors PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_secure_vectors COMMAND test_secure_vectors)

        # Test executable: test_data_secure_roundtrip
        add_executable(test_data_secure_roundtrip
            test/unit/test_data_secure_roundtrip.cpp
        )
        target_link_libraries(test_data_secure_roundtrip PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_data_secure_roundtrip COMMAND test_data_secure_roundtrip)

        # Test executable: test_netip_secure_wrapper_vectors
        add_executable(test_netip_secure_wrapper_vectors
            test/unit/test_netip_secure_wrapper_vectors.cpp
        )
        target_link_libraries(test_netip_secure_wrapper_vectors PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_netip_secure_wrapper_vectors COMMAND test_netip_secure_wrapper_vectors)
        set_tests_properties(test_netip_secure_wrapper_vectors PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )

        # Test executable: test_key_derivation
        add_executable(test_key_derivation
            test/unit/test_key_derivation.cpp
        )
        target_link_libraries(test_key_derivation PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_key_derivation COMMAND test_key_derivation)

        # Additional secure corner-case tests
        add_executable(test_data_secure_replay
            test/unit/test_data_secure_replay.cpp
        )
        target_link_libraries(test_data_secure_replay PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_data_secure_replay COMMAND test_data_secure_replay)

        add_executable(test_data_secure_seqwrap
            test/unit/test_data_secure_seqwrap.cpp
        )
        target_link_libraries(test_data_secure_seqwrap PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_data_secure_seqwrap COMMAND test_data_secure_seqwrap)

        add_executable(test_data_secure_tamper
            test/unit/test_data_secure_tamper.cpp
        )
        target_link_libraries(test_data_secure_tamper PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_data_secure_tamper COMMAND test_data_secure_tamper)

    else()
        message(STATUS "KNX Secure disabled: skipping secure unit tests (test_secure_vectors, test_key_derivation)")
    endif()

    # Test executable: test_ip_physical (UDP loopback)
    add_executable(test_ip_physical
        test/unit/test_ip_physical.cpp
    )
    target_link_libraries(test_ip_physical PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ip_physical COMMAND test_ip_physical)

    # Test executable: test_ip_physical_config (config hooks)
    add_executable(test_ip_physical_config
        test/unit/test_ip_physical_config.cpp
    )
    target_link_libraries(test_ip_physical_config PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ip_physical_config COMMAND test_ip_physical_config)

    # Test executable: test_cemi (cEMI encode/decode)
    add_executable(test_cemi
        test/unit/test_cemi.cpp
    )
    target_link_libraries(test_cemi PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_cemi COMMAND test_cemi)

    # Test executable: test_ets_config_loader
    add_executable(test_ets_config_loader
        test/unit/test_ets_config_loader.cpp
    )
    target_link_libraries(test_ets_config_loader PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ets_config_loader COMMAND test_ets_config_loader)

    # Test executable: test_ets_format_validation
    add_executable(test_ets_format_validation
        test/unit/test_ets_format_validation.cpp
    )
    target_link_libraries(test_ets_format_validation PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ets_format_validation COMMAND test_ets_format_validation)

    # Test executable: test_netip_tunneling (mock gateway)
    add_executable(test_netip_tunneling
        test/unit/test_netip_tunneling.cpp
    )
    target_link_libraries(test_netip_tunneling PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_tunneling COMMAND test_netip_tunneling)

    add_executable(test_netip_tunneling_session_client
        test/unit/test_netip_tunneling_session_client.cpp
    )
    target_link_libraries(test_netip_tunneling_session_client PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_tunneling_session_client COMMAND test_netip_tunneling_session_client)

    add_executable(test_netip_tunneling_server_endpoint
        test/unit/test_netip_tunneling_server_endpoint.cpp
    )
    target_link_libraries(test_netip_tunneling_server_endpoint PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_tunneling_server_endpoint COMMAND test_netip_tunneling_server_endpoint)

    add_executable(test_tp1_ip_interface_bridge
        test/unit/test_tp1_ip_interface_bridge.cpp
    )
    target_link_libraries(test_tp1_ip_interface_bridge PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_ip_interface_bridge COMMAND test_tp1_ip_interface_bridge)

    add_executable(test_netip_header_codec
        test/unit/test_netip_header_codec.cpp
    )
    target_link_libraries(test_netip_header_codec PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_header_codec COMMAND test_netip_header_codec)

    add_executable(test_netip_control_packet_codec
        test/unit/test_netip_control_packet_codec.cpp
    )
    target_link_libraries(test_netip_control_packet_codec PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_control_packet_codec COMMAND test_netip_control_packet_codec)

    add_executable(test_netip_gateway_discovery_codec
        test/unit/test_netip_gateway_discovery_codec.cpp
    )
    target_link_libraries(test_netip_gateway_discovery_codec PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_gateway_discovery_codec COMMAND test_netip_gateway_discovery_codec)

    add_executable(test_netip_secure_session_bootstrap
        test/unit/test_netip_secure_session_bootstrap.cpp
    )
    target_link_libraries(test_netip_secure_session_bootstrap PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_secure_session_bootstrap COMMAND test_netip_secure_session_bootstrap)

    add_executable(test_netip_udp_datagram_channel
        test/unit/test_netip_udp_datagram_channel.cpp
    )
    target_link_libraries(test_netip_udp_datagram_channel PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_udp_datagram_channel COMMAND test_netip_udp_datagram_channel)

    add_executable(test_netip_device_management_client
        test/unit/test_netip_device_management_client.cpp
    )
    target_link_libraries(test_netip_device_management_client PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_device_management_client COMMAND test_netip_device_management_client)

    add_executable(test_netip_device_management_session
        test/unit/test_netip_device_management_session.cpp
    )
    target_link_libraries(test_netip_device_management_session PRIVATE
        knx_core
    )
    add_test(NAME test_netip_device_management_session COMMAND test_netip_device_management_session)
    set_tests_properties(test_netip_device_management_session PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_executable(test_netip_device_management_procedures
        test/unit/test_netip_device_management_procedures.cpp
    )
    target_link_libraries(test_netip_device_management_procedures PRIVATE
        knx_core
    )
    add_test(NAME test_netip_device_management_procedures COMMAND test_netip_device_management_procedures)
    set_tests_properties(test_netip_device_management_procedures PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_executable(test_netip_device_management_commissioner
        test/unit/test_netip_device_management_commissioner.cpp
    )
    target_link_libraries(test_netip_device_management_commissioner PRIVATE
        knx_core
    )
    add_test(NAME test_netip_device_management_commissioner COMMAND test_netip_device_management_commissioner)
    set_tests_properties(test_netip_device_management_commissioner PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )

    add_executable(test_netip_tcp_frame_channel
        test/unit/test_netip_tcp_frame_channel.cpp
    )
    target_link_libraries(test_netip_tcp_frame_channel PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_tcp_frame_channel COMMAND test_netip_tcp_frame_channel)


    # Test executable: test_netip_routing (codec)
    add_executable(test_netip_routing
        test/unit/test_netip_routing.cpp
    )
    target_link_libraries(test_netip_routing PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_routing COMMAND test_netip_routing)

    # Test executable: test_netip_security_plumbing (transport security wrapper wiring)
    add_executable(test_netip_security_plumbing
        test/unit/test_netip_security_plumbing.cpp
    )
    target_link_libraries(test_netip_security_plumbing PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_security_plumbing COMMAND test_netip_security_plumbing)

    if(KNX_SECURE_ENABLED)
        # Test executable: test_netip_secure_timernotify_vector (vector from Wireshark/KNXUltimate)
        add_executable(test_netip_secure_timernotify_vector
            test/unit/test_netip_secure_timernotify_vector.cpp
        )
        target_link_libraries(test_netip_secure_timernotify_vector PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_netip_secure_timernotify_vector COMMAND test_netip_secure_timernotify_vector)
        set_tests_properties(test_netip_secure_timernotify_vector PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )

        # Test executable: test_netip_secure_session_vectors (KNXUltimate-generated vectors)
        add_executable(test_netip_secure_session_vectors
            test/unit/test_netip_secure_session_vectors.cpp
        )
        target_link_libraries(test_netip_secure_session_vectors PRIVATE
            knx_core
        )
        add_test(NAME test_netip_secure_session_vectors COMMAND test_netip_secure_session_vectors)
        set_tests_properties(test_netip_secure_session_vectors PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )

        # Test executable: test_netip_secure_tunneling_handshake (scripted TCP handshake with vectors)
        add_executable(test_netip_secure_tunneling_handshake
            test/unit/test_netip_secure_tunneling_handshake.cpp
        )
        target_link_libraries(test_netip_secure_tunneling_handshake PRIVATE
            knx_core
        )
        add_test(NAME test_netip_secure_tunneling_handshake COMMAND test_netip_secure_tunneling_handshake)
        set_tests_properties(test_netip_secure_tunneling_handshake PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()

    # Integration test: adapter wiring with Tp1DataLinkLayer
    add_executable(test_ip_tunneling_adapter
        test/integration/test_ip_tunneling_adapter.cpp
    )
    target_link_libraries(test_ip_tunneling_adapter PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ip_tunneling_adapter COMMAND test_ip_tunneling_adapter)
    set_tests_properties(test_ip_tunneling_adapter PROPERTIES LABELS "integration;Long")

    # Integration smoke test: TP1 <-> IP interface bridge path
    add_executable(test_tp1_ip_interface
        test/integration/test_tp1_ip_interface.cpp
    )
    target_link_libraries(test_tp1_ip_interface PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_ip_interface COMMAND test_tp1_ip_interface)
    set_tests_properties(test_tp1_ip_interface PROPERTIES LABELS "integration")

    # Integration test: KNXnet/IP routing multicast (endpoint)
    add_executable(test_netip_routing_multicast
        test/integration/test_netip_routing_multicast.cpp
    )
    target_link_libraries(test_netip_routing_multicast PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_netip_routing_multicast COMMAND test_netip_routing_multicast)

    if(KNX_SECURE_ENABLED)
        # Integration test: KNX/IP Secure routing multicast (SecureWrapper 0x0950)
        add_executable(test_netip_routing_multicast_secure
            test/integration/test_netip_routing_multicast_secure.cpp
        )
        target_link_libraries(test_netip_routing_multicast_secure PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_netip_routing_multicast_secure COMMAND test_netip_routing_multicast_secure)

        # Integration test: Tp1DataLinkLayer over KNXnet/IP Secure tunneling physical adapter
        add_executable(test_ip_secure_tunneling_physical
            test/integration/test_ip_secure_tunneling_physical.cpp
        )
        target_link_libraries(test_ip_secure_tunneling_physical PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_ip_secure_tunneling_physical COMMAND test_ip_secure_tunneling_physical)
        set_tests_properties(test_ip_secure_tunneling_physical PROPERTIES
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endif()

    # Integration test: Tp1DataLinkLayer over KNXnet/IP routing physical adapter
    add_executable(test_ip_routing_physical
        test/integration/test_ip_routing_physical.cpp
    )
    target_link_libraries(test_ip_routing_physical PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ip_routing_physical COMMAND test_ip_routing_physical)
    set_tests_properties(test_ip_routing_physical PROPERTIES LABELS "tp1;integration")

    # Integration test: TP1 -> IP routing forwarding via coupler
    add_executable(test_tp1_to_ip_routing_coupler
        test/integration/test_tp1_to_ip_routing_coupler.cpp
    )
    target_link_libraries(test_tp1_to_ip_routing_coupler PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_tp1_to_ip_routing_coupler COMMAND test_tp1_to_ip_routing_coupler)
    set_tests_properties(test_tp1_to_ip_routing_coupler PROPERTIES LABELS "tp1;integration")

    add_executable(test_coupler_stack_port
        test/integration/test_coupler_stack_port.cpp
    )
    target_link_libraries(test_coupler_stack_port PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_coupler_stack_port COMMAND test_coupler_stack_port)
    set_tests_properties(test_coupler_stack_port PROPERTIES LABELS "tp1;integration")

    add_executable(test_two_port_coupler
        test/integration/test_two_port_coupler.cpp
    )
    target_link_libraries(test_two_port_coupler PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_two_port_coupler COMMAND test_two_port_coupler)
    set_tests_properties(test_two_port_coupler PROPERTIES LABELS "tp1;integration")

    if(BUILD_TESTS)
        # --- Test labels: group tests by intent so `ctest -L <label>` works
        # Unit tests (files under test/unit)
        set(UNIT_TESTS
            test_types
            test_dpt
            test_manchester_codec
            test_tp1_frame_codec
            test_frame_encoding
            test_frame_codec_errors
            test_decoder_robustness
            test_result
            test_config_validation
            test_product_model
            test_product_compiler
            test_product_export
            test_product_endpoint_public_surface
            test_product_endpoint_semantics
            test_product_endpoint_policies
            test_product_commissioned_surface
            test_product_runtime_core
            test_logging
            test_datalink_edge_cases
            test_tp1_datalink_frame_pool
            test_tp1_mac_controller
            test_virtual_time_engine
            test_virtual_test_clock
            test_virtual_platform
            test_virtual_tp1_test_runtime
            test_timer_gpio_hal_virtual
            test_tp1_bitbang_virtual_bus
            test_tp1_bitbang_virtual_faults
            test_tp1_virtual_trace_replay
            test_tp1_datalink_virtual_timing
            test_tpuart_medium_backend_adapter
            test_tpuart_mac_physical
            test_physical_factory_tp1
            test_cemi
            test_ets_config_loader
            test_ets_format_validation
            test_netip_tunneling
            test_netip_tunneling_session_client
            test_netip_header_codec
            test_netip_udp_datagram_channel
            test_netip_tcp_frame_channel
            test_netip_routing
            test_netip_security_plumbing
            test_secure_vectors
            test_data_secure_roundtrip
            test_netip_secure_wrapper_vectors
            test_key_derivation
            test_data_secure_roundtrip
            test_data_secure_replay
            test_data_secure_seqwrap
            test_data_secure_tamper
            test_ip_physical
            test_ip_physical_config
            test_netip_secure_timernotify_vector
            test_netip_secure_session_vectors
            test_netip_secure_session_bootstrap
            test_netip_secure_tunneling_handshake
            test_transport_connection
            test_connection_state
            test_connection_table
            test_netip_secure_wrapper_vectors
        )

        foreach(t IN LISTS UNIT_TESTS)
            if(TARGET ${t})
                set_property(TEST ${t} APPEND PROPERTY LABELS "unit")
            endif()
        endforeach()

        # Integration tests (files under test/integration)
        set(INTEGRATION_TESTS
            test_tp1_integration
            test_ets_roundtrip
            test_knxprog_importer
            test_knx_stack_e2e
            test_transport_application
            test_ets_roundtrip
            test_ip_tunneling_adapter
            test_netip_routing_multicast
            test_netip_routing_multicast_secure
            test_ip_secure_tunneling_physical
            test_ip_routing_physical
            test_tp1_to_ip_routing_coupler
            test_ip_tp1_routing
            test_knxnetip_integration
            test_bau_onwire
            test_product_runtime
        )

        foreach(t IN LISTS INTEGRATION_TESTS)
            if(TARGET ${t})
                set_property(TEST ${t} APPEND PROPERTY LABELS "integration")
            endif()
        endforeach()

    endif()

    # Interop helper: KNstaX tunnelling peer (used by Python interop harness)
    add_executable(knstax_tunnel_peer
        interop/knstax_peer/knstax_tunnel_peer.cpp
    )
    target_link_libraries(knstax_tunnel_peer PRIVATE
        knx_core
    )

    # Interop helper: KNstaX routing peer (used by Python interop harness)
    add_executable(knstax_routing_peer
        interop/knstax_peer/knstax_routing_peer.cpp
    )
    target_link_libraries(knstax_routing_peer PRIVATE
        knx_core
    )

    option(ENABLE_INTEROP_TESTS "Build and register the interop Python harness tests" OFF)
    if(ENABLE_INTEROP_TESTS)
        set(INTEROP_BIN_DIR ${CMAKE_BINARY_DIR}/interop/bin)
        file(MAKE_DIRECTORY ${INTEROP_BIN_DIR})

        # Place interop peer binaries in a stable location so tests can be
        # run reproducibly by CI (CTest will export env vars pointing here).
        set_target_properties(knstax_tunnel_peer PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${INTEROP_BIN_DIR})
        set_target_properties(knstax_routing_peer PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${INTEROP_BIN_DIR})

        find_package(Python3 COMPONENTS Interpreter REQUIRED)

        # Detect whether pytest is available in the selected Python interpreter.
        execute_process(COMMAND ${Python3_EXECUTABLE} -m pytest --version
                        RESULT_VARIABLE PYTEST_EXIT_CODE
                        OUTPUT_QUIET ERROR_QUIET)
        if(PYTEST_EXIT_CODE EQUAL 0)
            add_test(NAME interop_pytest
                COMMAND ${Python3_EXECUTABLE} -m pytest -q ${CMAKE_SOURCE_DIR}/interop/python/tests ${CMAKE_SOURCE_DIR}/tools/test_knxprod2profile_param_test.py)

            set_tests_properties(interop_pytest PROPERTIES
                ENVIRONMENT "KNSTAX_TUNNEL_PEER_BIN=${INTEROP_BIN_DIR}/knstax_tunnel_peer;KNSTAX_ROUTING_PEER_BIN=${INTEROP_BIN_DIR}/knstax_routing_peer"
                LABELS "Interop;Integration;Long"
                # Interop harness may take longer than unit tests; allow up to 15 minutes
                TIMEOUT 900
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            )
        else()
            message(WARNING "Python interpreter found (${Python3_EXECUTABLE}) but pytest not available; skipping interop_pytest registration.")
        endif()
    endif()

    # Integration test: IP to TP1 routing
    add_executable(test_ip_tp1_routing
        test/integration/test_ip_tp1_routing.cpp
    )
    target_link_libraries(test_ip_tp1_routing PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_ip_tp1_routing COMMAND test_ip_tp1_routing)
    set_tests_properties(test_ip_tp1_routing PROPERTIES LABELS "tp1;integration")

    # Integration test: InterfaceObjectManager persistence (host FilePersistence)
    add_executable(test_interface_object_manager_persistence
        test/integration/test_interface_object_manager_persistence.cpp
    )
    target_link_libraries(test_interface_object_manager_persistence PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_interface_object_manager_persistence COMMAND test_interface_object_manager_persistence)

    # Test executable: test_transport_retransmission (Phase 1A.3/1A.4)
    add_executable(test_transport_retransmission
        test/unit/test_transport_retransmission.cpp
    )
    target_link_libraries(test_transport_retransmission PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_transport_retransmission COMMAND test_transport_retransmission)

    # Test executable: test_device_descriptor (Phase 1B.1)
    add_executable(test_device_descriptor
        test/unit/test_device_descriptor.cpp
    )
    target_link_libraries(test_device_descriptor PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_device_descriptor COMMAND test_device_descriptor)

    # Test executable: test_property_services (Phase 1B.2)
    add_executable(test_property_services
        test/unit/test_property_services.cpp
    )
    target_link_libraries(test_property_services PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_property_services COMMAND test_property_services)

    # Test executable: test_property_ext_services (extended property services)
    add_executable(test_property_ext_services
        test/unit/test_property_ext_services.cpp
    )
    target_link_libraries(test_property_ext_services PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_property_ext_services COMMAND test_property_ext_services)

    # Test executable: test_interface_object_manager
    add_executable(test_interface_object_manager
        test/unit/test_interface_object_manager.cpp
    )
    target_link_libraries(test_interface_object_manager PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_interface_object_manager COMMAND test_interface_object_manager)

    # Test executable: test_interface_object_manager_instances
    add_executable(test_interface_object_manager_instances
        test/unit/test_interface_object_manager_instances.cpp
    )
    target_link_libraries(test_interface_object_manager_instances PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_interface_object_manager_instances COMMAND test_interface_object_manager_instances)

    # Test executable: test_object_property_compliance
    add_executable(test_object_property_compliance
        test/unit/test_object_property_compliance.cpp
    )
    target_link_libraries(test_object_property_compliance PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_object_property_compliance COMMAND test_object_property_compliance)

    # Test executable: test_manifest_live_coverage (Assembly B)
    add_executable(test_manifest_live_coverage
        test/unit/test_manifest_live_coverage.cpp
    )
    target_link_libraries(test_manifest_live_coverage PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_manifest_live_coverage COMMAND test_manifest_live_coverage)

    # Test executable: test_memory_service (Phase 1B.3)
    add_executable(test_memory_service
        test/unit/test_memory_service.cpp
    )
    target_link_libraries(test_memory_service PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_memory_service COMMAND test_memory_service)

    # Test executable: test_authorization_service (Phase 1B.4)
    add_executable(test_authorization_service
        test/unit/test_authorization_service.cpp
    )
    target_link_libraries(test_authorization_service PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_authorization_service COMMAND test_authorization_service)

    # Test executable: test_restart_service (Phase 1B.5)
    add_executable(test_restart_service
        test/unit/test_restart_service.cpp
    )
    target_link_libraries(test_restart_service PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_restart_service COMMAND test_restart_service)

    # Test executable: test_application_layer (Phase 1B.6)
    add_executable(test_application_layer
        test/unit/test_application_layer.cpp
    )
    target_link_libraries(test_application_layer PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_application_layer COMMAND test_application_layer)

    # Test executable: test_application_integration (Phase 1B.7)
    add_executable(test_application_integration
        test/integration/test_application_integration.cpp
    )
    target_link_libraries(test_application_integration PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_application_integration COMMAND test_application_integration)

    add_executable(test_application_virtual_lifecycle
        test/integration/test_application_virtual_lifecycle.cpp
    )
    target_link_libraries(test_application_virtual_lifecycle PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_application_virtual_lifecycle COMMAND test_application_virtual_lifecycle)
    set_tests_properties(test_application_virtual_lifecycle PROPERTIES LABELS "tp1;integration;virtual;tp1-virtual")

    # Test executable: test_device_object (Phase 1C.1)
    add_executable(test_device_object
        test/unit/test_device_object.cpp
    )
    target_link_libraries(test_device_object PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_device_object COMMAND test_device_object)

    # Test executable: test_inplace_function
    add_executable(test_inplace_function
        test/unit/test_inplace_function.cpp
    )
    target_link_libraries(test_inplace_function PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_inplace_function COMMAND test_inplace_function)
    set_tests_properties(test_inplace_function PROPERTIES LABELS "unit")

    # Test executable: test_application_program_object (Phase 1C.2)
    add_executable(test_application_program_object
        test/unit/test_application_program_object.cpp
    )
    target_link_libraries(test_application_program_object PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_application_program_object COMMAND test_application_program_object)

    # Test executable: test_address_association_tables (Phase 1C.3)
    add_executable(test_address_association_tables
        test/unit/test_address_association_tables.cpp
    )
    target_link_libraries(test_address_association_tables PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_address_association_tables COMMAND test_address_association_tables)

    # Test executable: test_group_object_table (Phase 1C.4)
    add_executable(test_group_object_table
        test/unit/test_group_object_table.cpp
    )
    target_link_libraries(test_group_object_table PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_group_object_table COMMAND test_group_object_table)

    # Test executable: test_security_interface_object (Phase 1C.5)
    add_executable(test_security_interface_object
        test/unit/test_security_interface_object.cpp
    )
    target_link_libraries(test_security_interface_object PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_security_interface_object COMMAND test_security_interface_object)

    # Test executable: test_routing_table (Phase 2.1)
    add_executable(test_routing_table
        test/unit/test_routing_table.cpp
    )
    target_link_libraries(test_routing_table PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_routing_table COMMAND test_routing_table)

    # Test executable: test_filter_table (Phase 2.1)
    add_executable(test_filter_table
        test/unit/test_filter_table.cpp
    )
    target_link_libraries(test_filter_table PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_filter_table COMMAND test_filter_table)

    # Test executable: test_dpt2_3_4 (Phase 3.1)
    add_executable(test_dpt2_3_4
        test/unit/test_dpt2_3_4.cpp
    )
    target_link_libraries(test_dpt2_3_4 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt2_3_4 COMMAND test_dpt2_3_4)

    # Test executable: test_dpt6_7_8 (Phase 3.1)
    add_executable(test_dpt6_7_8
        test/unit/test_dpt6_7_8.cpp
    )
    target_link_libraries(test_dpt6_7_8 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt6_7_8 COMMAND test_dpt6_7_8)

    # Test executable: test_dpt10_11_19 (Phase 3.2)
    add_executable(test_dpt10_11_19
        test/unit/test_dpt10_11_19.cpp
    )
    target_link_libraries(test_dpt10_11_19 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt10_11_19 COMMAND test_dpt10_11_19)

    # Test executable: test_dpt12_13_14 (Phase 3.3)
    add_executable(test_dpt12_13_14
        test/unit/test_dpt12_13_14.cpp
    )
    target_link_libraries(test_dpt12_13_14 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt12_13_14 COMMAND test_dpt12_13_14)

    # Test executable: test_dpt16_17 (Phase 3.3)
    add_executable(test_dpt16_17
        test/unit/test_dpt16_17.cpp
    )
    target_link_libraries(test_dpt16_17 PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt16_17 COMMAND test_dpt16_17)

    # Test executable: test_dpt_extended (Extended DPT types - NEW)
    add_executable(test_dpt_extended
        test/unit/test_dpt_extended.cpp
    )
    target_link_libraries(test_dpt_extended PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_dpt_extended COMMAND test_dpt_extended)

    # Test executable: test_error_injection (Error handling - NEW)
    add_executable(test_error_injection
        test/unit/test_error_injection.cpp
    )
    target_link_libraries(test_error_injection PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_error_injection COMMAND test_error_injection)

    # RF physical layer is not shipped by default.

    # Test executable: test_protocol_conformance (Conformance - NEW)
    add_executable(test_protocol_conformance
        test/integration/test_protocol_conformance.cpp
    )
    target_link_libraries(test_protocol_conformance PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_protocol_conformance COMMAND test_protocol_conformance)

    # Test executable: test_knxnetip_discovery (KNXnet/IP Discovery - NEW)
    add_executable(test_knxnetip_discovery
        test/unit/test_knxnetip_discovery.cpp
    )
    target_link_libraries(test_knxnetip_discovery PRIVATE
        knx_core
        unity
        pthread
    )
    add_test(NAME test_knxnetip_discovery COMMAND test_knxnetip_discovery)

    add_executable(test_netip_gateway_discovery_client
        test/unit/test_netip_gateway_discovery_client.cpp
    )
    target_link_libraries(test_netip_gateway_discovery_client PRIVATE
        knx_core
        unity
        pthread
    )
    add_test(NAME test_netip_gateway_discovery_client COMMAND test_netip_gateway_discovery_client)
    set_tests_properties(test_netip_gateway_discovery_client PROPERTIES LABELS "unit")

    # Test executable: test_knxnetip_integration (KNXnet/IP Integration - NEW)
    add_executable(test_knxnetip_integration
        test/integration/test_knxnetip_integration.cpp
    )
    target_link_libraries(test_knxnetip_integration PRIVATE
        knx_core
        unity
        pthread
    )
    add_test(NAME test_knxnetip_integration COMMAND test_knxnetip_integration)

    # Test executable: test_bau_onwire (BAU on-wire golden tests - NEW)
    add_executable(test_bau_onwire
        test/integration/test_bau_onwire.cpp
    )
    target_link_libraries(test_bau_onwire PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_bau_onwire COMMAND test_bau_onwire)

    # Test executable: test_product_runtime (M1 rewrite-lane started runtime proof)
    add_executable(test_product_runtime
        test/integration/test_product_runtime.cpp
    )
    target_link_libraries(test_product_runtime PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_product_runtime COMMAND test_product_runtime)

    # Test executable: test_conformance_evidence_commissioned (P4.3-cp — commissioned path)
    add_executable(test_conformance_evidence_commissioned
        test/conformance/test_conformance_evidence_commissioned.cpp
    )
    target_link_libraries(test_conformance_evidence_commissioned PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_conformance_evidence_commissioned COMMAND test_conformance_evidence_commissioned)
    set_tests_properties(test_conformance_evidence_commissioned PROPERTIES LABELS "conformance")
    set_property(TEST test_conformance_evidence_commissioned APPEND PROPERTY LABELS "sdk-public-surface")

    # Test executable: test_transmit_policy_ets_parameters (ETS params → transmit policy)
    add_executable(test_transmit_policy_ets_parameters
        test/integration/test_transmit_policy_ets_parameters.cpp
    )
    target_link_libraries(test_transmit_policy_ets_parameters PRIVATE
        knx_core
        unity
    )
    add_test(NAME test_transmit_policy_ets_parameters COMMAND test_transmit_policy_ets_parameters)
    set_tests_properties(test_transmit_policy_ets_parameters PROPERTIES LABELS "integration")

    if(BUILD_TESTS)
        set(KNX_SDK_FLAKE_TEST_TARGETS
            test_virtual_platform
            test_virtual_tp1_test_runtime
            test_tp1_bitbang_virtual_bus
            test_tp1_virtual_trace_replay
            test_tp1_datalink_virtual_timing
        )

        add_custom_target(knx_sdk_virtual_flake_lane
            COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure --repeat until-fail:3 -L sdk-flake-candidate
            USES_TERMINAL
            COMMENT "Running KNstaX virtual-platform flake lane"
        )
        add_dependencies(knx_sdk_virtual_flake_lane ${KNX_SDK_FLAKE_TEST_TARGETS})
    endif()

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

        add_executable(ip_device
            examples/ip_device/main.cpp
        )
        target_link_libraries(ip_device PRIVATE
            knx_core
        )

        add_executable(ip_routing_device
            examples/ip_routing_device/main.cpp
        )
        target_link_libraries(ip_routing_device PRIVATE
            knx_core
        )

        add_executable(tp1_ip_interface
            examples/tp1_ip_interface/main.cpp
        )
        target_link_libraries(tp1_ip_interface PRIVATE
            knx_core
        )

        add_executable(test_knxnetip_routing_sniffer_e2e
            test/integration/test_knxnetip_routing_sniffer_e2e.cpp
        )
        target_link_libraries(test_knxnetip_routing_sniffer_e2e PRIVATE
            knx_core
            unity
        )
        add_test(NAME test_knxnetip_routing_sniffer_e2e
            COMMAND test_knxnetip_routing_sniffer_e2e $<TARGET_FILE:knxnetip_routing_sniffer>
        )

        add_custom_target(knx_sdk_example_contracts
            DEPENDS
                tp1_thermostat
                tp1_line_coupler
                knxnetip_routing_sniffer
                ip_device
                ip_routing_device
                tp1_ip_interface
        )

        # Build-time .knxprod generation, demonstrating the single-source-of-truth
        # DX: product.hpp → .knxprod.xml with no hand-written ETS file. Driven
        # from the ESP-IDF switch example's definition, which proves the point
        # that matters — the same header compiles for target firmware and for
        # the host exporter.
        knx_commissioned_product(tp1_switch_ets
            DEFINITION  ${CMAKE_SOURCE_DIR}/examples/esp_idf_switch/main/product.hpp
            SYMBOL      tp1_switch::kProduct
        )

        if(BUILD_TESTS)
            add_custom_target(knx_sdk_release_gate
                COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure -L sdk-public-surface
                COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure --repeat until-fail:3 -L sdk-flake-candidate
                USES_TERMINAL
                COMMENT "Running KNstaX SDK release gate"
            )
            add_dependencies(knx_sdk_release_gate
                knx_sdk_public_header_contracts
                knx_sdk_example_contracts
                test_product_endpoint_public_surface
                test_product_commissioned_surface
                # Carries the "sdk-public-surface" label the gate filters on, so
                # the gate ran it without ever building it and failed with
                # "Unable to find executable" on a clean tree.
                test_conformance_evidence_commissioned
                ${KNX_SDK_FLAKE_TEST_TARGETS}
            )
        endif()
    endif()

    # Benchmark executable: benchmark_secure (only if secure is enabled)
    if(KNX_SECURE_ENABLED)
        add_executable(benchmark_secure
            benchmark/benchmark_secure.cpp
        )
        target_link_libraries(benchmark_secure PRIVATE
            knx_core
        )
    endif()

    add_executable(benchmark_tp1
        benchmark/benchmark_tp1.cpp
    )
    target_link_libraries(benchmark_tp1 PRIVATE
        knx_core
    )

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
