// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file product.hpp
 * @brief TP1 Switch product definition — the single source of truth.
 *
 * This header is intentionally free of hardware, platform, and OS dependencies
 * so it can be compiled by:
 *   - the embedded firmware (main.cpp on target)
 *   - the knx_commissioned_product() host-side exporter (generates .knxprod.xml)
 *
 * Keep this file as a pure constexpr declaration with no side effects.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"

namespace tp1_switch {

using namespace knx;
using namespace knx::application;
using namespace knx::product;

// ── Logical port identifiers ─────────────────────────────────────────────────

enum class Port : uint16_t {
    RelayCommand = 0, ///< KNX group write → relay actuator
    RelayState   = 1, ///< KNX group read/response ← relay state feedback
};

// ── Product definition (endpoint layout + metadata) ──────────────────────────
//
// This is the ONLY place where:
//   - port keys and display names are declared
//   - DPT types are assigned
//   - ETS application program identity is set
//   - persistence policy is configured
//
// From this single constexpr the KNstaX build system derives:
//   1. The embedded runtime (via makeCommissionedProduct → startCommissionedProduct)
//   2. The ETS .knxprod catalogue entry (via knx_commissioned_product() CMake target)

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           semantics::SwitchCommand<Port::RelayCommand,
                                                    "relay_command",
                                                    "Relay Command">,
                           semantics::SwitchState<Port::RelayState,
                                                  "relay_state",
                                                  "Relay State">>(
        ProductIdentity{
            .productKey         = "tp1_switch",
            .productDisplayName = "TP1 Switch",
            .manufacturerId     = ManufacturerId(0x00FA),
            .medium             = endpoint::Medium::TP1,
            .applicationNumber  = 1,
            .applicationVersion = 1,
            .firmwareRevision   = 1,
            .maxApduLength      = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp1_switch",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }));

} // namespace tp1_switch
