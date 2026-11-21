// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file product.hpp
 * @brief The GD32F1 port example: a one-channel switch actuator
 *
 * One switch object and its status object. No hardware or OS dependencies,
 * so the same header can feed the ETS product exporter.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"

namespace example {

using namespace knx;
using namespace knx::product;

enum class Port : uint16_t {
    Switch = 0,
    Status = 1,
};

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           semantics::SwitchCommand<Port::Switch, "switch", "Switch">,
                           semantics::SwitchState<Port::Status, "status", "Status", false>>(
        ProductIdentity{
            .productKey = "gd32f1_example",
            .productDisplayName = "KNstaX GD32F1 example switch",
            .manufacturerId = ManufacturerId(0x00FA),  // KNstaX's example ID: not a registered product
            .medium = endpoint::Medium::TP1,
            .applicationNumber = 1,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 15,
        },
        PersistencePolicy{
            .namespacePrefix = "gd32f1_example",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

}  // namespace example
