// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file product.hpp
 * @brief Low-power TP1 contact sensor product definition.
 *
 * Pure constexpr endpoint metadata only. This file intentionally stays free of
 * hardware, RTOS, and platform code so the same declaration can back both the
 * firmware runtime and host-side exporter/tooling flows.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"

namespace esp_idf_low_power_sensor {

using namespace knx;
using namespace knx::application;
using namespace knx::product;

enum class Port : uint16_t {
    ContactState = 0,
};

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           // DPT 1.019 Window/Door, not the generic DPT 1.001
                           // switch: the sub-type is what tells a visualisation
                           // that "1" means open rather than "on".
                           semantics::WindowDoorState<Port::ContactState,
                                                      "contact_state",
                                                      "Contact State",
                                                      false>>(
        ProductIdentity{
            .productKey = "esp_idf_low_power_sensor",
            .productDisplayName = "TP1 Low Power Contact Sensor",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = endpoint::Medium::TP1,
            .applicationNumber = 9,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "lp_contact",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

} // namespace esp_idf_low_power_sensor