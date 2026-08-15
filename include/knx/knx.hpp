// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file knx.hpp
 * @brief Top-level KNstaX public entry surface for commissioned endpoint products.
 *
 * This is the recommended single-include entry point for firmware that uses the
 * commissioned-product model:
 *
 *   #include "knx/knx.hpp"
 *
 *   constexpr auto kProduct = makeCommissionedProduct(...);
 *
 *   auto result = startCommissionedProduct(platform, kProduct, std::move(bindings), backend);
 *
 * For the ETS export pipeline include knx/product/exporter.hpp separately in the
 * host-side export binary (not needed on the device).
 *
 * Platform-specific headers (freertos_platform.hpp, linux_platform.hpp, etc.) are
 * NOT included here because they contain embedded-only or host-only dependencies.
 * Include the appropriate platform header directly in your application entry point.
 */

#pragma once

// Version information.
// Keep in sync with KNX_VERSION_STRING in CMakeLists.txt and the `version`
// field of idf_component.yml.  The build system also passes KNX_STACK_VERSION
// as a compile definition; the guard keeps the two from colliding.
#define KNX_STACK_VERSION_MAJOR 0
#define KNX_STACK_VERSION_MINOR 2
#define KNX_STACK_VERSION_PATCH 0
#define KNX_STACK_VERSION_SUFFIX "alpha"
#ifndef KNX_STACK_VERSION
#define KNX_STACK_VERSION "0.2.0-alpha"
#endif

// Core types and configuration
#include "knx/config.hpp"
#include "knx/types.hpp"

// Platform abstraction interfaces (no platform-specific implementation included)
#include "knx/platform/platform.hpp"
#include "knx/platform/memory_interface.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/uart_interface.hpp"
#include "knx/platform/spi_interface.hpp"

// Commissioned endpoint product surface — the primary firmware authoring API.
// Pulls in endpoint definition, bindings, runtime, policies, semantics, and
// the startCommissionedProduct() factory.
#include "knx/product/commissioned_product.hpp"

// Expert helpers (group-address binding overrides, raw parameter injection)
// are intentionally NOT included here. Normal firmware should opt in to
// knx/product/commissioned_product_expert.hpp only when it truly needs the
// expert/demo-only surface.

// Fault model types (FaultCode, FaultInfo, FaultCallback).
#include "knx/product/fault_model.hpp"

/**
 * @namespace knx
 * @brief Main namespace for KNX Stack
 * 
 * All KNX stack components are in this namespace or sub-namespaces:
 * - knx::platform - Platform abstraction layer
 * - knx::datalink - Data link layer
 * - knx::physical - Physical layer
 * - knx::network - Network layer
 * - knx::transport - Transport layer
 * - knx::application - Application layer
 * - knx::security - Security layer
 * - knx::objects - Interface objects
 * - knx::bau - Bus Access Units
 * - knx::util - Utility functions
 */
namespace knx {

/**
 * @brief Get KNX stack version string.
 */
inline constexpr const char* getVersion() noexcept
{
    return KNX_STACK_VERSION;
}

/**
 * @brief Get KNX stack version components.
 */
inline constexpr void getVersionComponents(uint8_t& major, uint8_t& minor, uint8_t& patch) noexcept
{
    major = KNX_STACK_VERSION_MAJOR;
    minor = KNX_STACK_VERSION_MINOR;
    patch = KNX_STACK_VERSION_PATCH;
}

} // namespace knx
