// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file product.hpp
 * @brief KNXnet/IP Switch product definition — the single source of truth.
 *
 * Identical in structure to the TP1 switch product definition, but intended
 * for deployment over KNXnet/IP tunneling (e.g. a software KNX device running
 * on Linux or a gateway-connected embedded system).
 *
 * This header is intentionally free of hardware, platform, and OS dependencies.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"

namespace ip_device {

using namespace knx;
using namespace knx::application;
using namespace knx::product;

// ── Logical port identifiers ─────────────────────────────────────────────────

enum class Port : uint16_t {
    RelayCommand = 0, ///< KNX group write → relay actuator
    RelayState   = 1, ///< KNX group read/response ← relay state feedback
};

// ── Product definition (endpoint layout + metadata) ──────────────────────────

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           semantics::SwitchCommand<Port::RelayCommand,
                                                    "relay_command",
                                                    "Relay Command">,
                           semantics::SwitchState<Port::RelayState,
                                                  "relay_state",
                                                  "Relay State">>(
        ProductIdentity{
            .productKey = "ip_switch",
            .productDisplayName = "KNXnet/IP Switch Actuator",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = endpoint::Medium::IP_Tunneling,
            .applicationNumber = 42,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "ip_switch",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

} // namespace ip_device
