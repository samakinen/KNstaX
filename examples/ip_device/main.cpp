// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNXnet/IP Switch Actuator — firmware entry point.
 *
 * Demonstrates the commissioned product API over KNXnet/IP tunneling.
 * The product layout is identical to the TP1 switch; only the transport
 * changes.  No physical layer assembly or factory include is required —
 * pass IpTunnelingOptions and the stack handles the rest.
 *
 * DX properties demonstrated here:
 *   - IpTunnelingOptions: host + port only; network is taken from Platform.
 *   - Lifecycle callbacks work identically to TP1 products.
 *   - transportPath() / supportTier() classify the active transport at runtime.
 *   - SecureCommissioningOptions can be added to options.secure without
 *     any other code change.
 *
 * To point at a real KNXnet/IP gateway, replace the host address below with
 * the gateway IP (or parse it from argv / NVS / config file).
 */

#include "product.hpp"

#include "knx/platform/linux_platform.hpp"
#include "knx/util/log.hpp"

#include <cstdlib>
#include <cstring>

using namespace knx;
using namespace ip_device;

namespace {

IpAddress parseIpOrDefault(const char* envName, const char* fallback)
{
    const char* value = std::getenv(envName);
    if (value == nullptr || std::strlen(value) == 0) {
        return IpAddress::fromString(fallback);
    }
    return IpAddress::fromString(value);
}

} // namespace

int main() {
    platform::LinuxPlatform platform;

    bool relayOn = false;
    bool relayStateDirty = false;

    // Runtime-configurable KNXnet/IP gateway address.
    // In production firmware this typically comes from provisioned config.
    const IpAddress kGatewayHost = parseIpOrDefault("KNX_GATEWAY_HOST", "192.168.1.254");

    auto appResult = startCommissionedProduct(
        platform,
        kProduct,
        makeCommissionedBindings(kProduct)
            .onCommand<Port::RelayCommand>([&](bool on) {
                relayOn = on;
                relayStateDirty = true;
                KNX_LOGI("IpSwitch", "Relay: %s", relayOn ? "ON" : "OFF");
            })
            .provideState<Port::RelayState>([&]() {
                return relayOn;
            })
            .onLifecycleChanged([](product::DeviceLifecycleState state) {
                switch (state) {
                    case product::DeviceLifecycleState::Uncommissioned:
                        KNX_LOGI("IpSwitch", "Lifecycle: Uncommissioned");
                        break;
                    case product::DeviceLifecycleState::Commissioning:
                        KNX_LOGI("IpSwitch", "Lifecycle: Programming mode active");
                        break;
                    case product::DeviceLifecycleState::Operational:
                        KNX_LOGI("IpSwitch", "Lifecycle: Operational");
                        break;
                }
            })
            .onProgrammingModeChanged([](bool enabled) {
                KNX_LOGI("IpSwitch", "Programming mode: %s", enabled ? "ON" : "OFF");
            }),
        IpTunnelingOptions{
            .host = kGatewayHost,
            // .port = NetIpPort(3671),   // default — omit unless non-standard
            // .secure = SecureCommissioningOptions{ .enabled = true, ... },
        });

    if (appResult.isError()) {
        KNX_LOGE("IpSwitch", "Start failed: %d", static_cast<int>(appResult.error()));
        return 1;
    }

    auto app = std::move(appResult.value());

    KNX_LOGI("IpSwitch", "Transport: %s, tier: %s",
             app->transportPath() == product::CommissionedTransportPath::IpTunnelingManaged
                 ? "IpTunnelingManaged" : "other",
             app->supportTier() == product::CommissionedSupportTier::Functional
                 ? "Functional" : "other");

    for (;;) {
        app->loop();

        if (relayStateDirty && app->lifecycleState() == product::DeviceLifecycleState::Operational) {
            relayStateDirty = false;
            const auto publish = app->publish<Port::RelayState>(relayOn);
            if (publish.isError()) {
                KNX_LOGW("IpSwitch", "State publish failed: %d", static_cast<int>(publish.error()));
            }
        }

        platform.delay(5);
    }
}
