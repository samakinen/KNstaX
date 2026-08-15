// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNXnet/IP Tunneling Device example (ETS commissioned path).
 *
 * This example intentionally retires the legacy raw TP1/IP bridge authoring
 * path. It demonstrates only commissioned startup via:
 *
 *   startCommissionedProduct(...) + IpTunnelingOptions
 *
 * Firmware code does not hardcode KNX addresses. ETS commissions the device
 * (individual address, group addresses, parameters), and KNstaX persists that
 * commissioned state automatically.
 */

#include "knx/product/commissioned_product.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/util/log.hpp"

#include <cstdlib>
#include <cstring>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

namespace {

enum class InterfacePort : uint16_t {
    RelayCommand = 0,
    RelayState = 1,
};

constexpr auto kInterfaceProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<
            InterfacePort,
            semantics::SwitchCommand<InterfacePort::RelayCommand, "relay_command", "Relay Command">,
            semantics::SwitchState<InterfacePort::RelayState, "relay_state", "Relay State">>(
            ProductIdentity{
                .productKey = "tp1_ip_interface",
                .productDisplayName = "KNXnet/IP Tunneling Device",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::IP_Tunneling,
                .applicationNumber = 8,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 254,
            },
            PersistencePolicy{
                .namespacePrefix = "tp1_ip_interface",
                .schemaVersion = 1,
                .persistKnxState = true,
            }));

uint16_t parsePortOrDefault(const char* envName, uint16_t fallback)
{
    const char* value = std::getenv(envName);
    if (value == nullptr || std::strlen(value) == 0) {
        return fallback;
    }

    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0 || parsed > 65535UL) {
        return fallback;
    }

    return static_cast<uint16_t>(parsed);
}

IpAddress parseIpOrDefault(const char* envName, const char* fallback)
{
    const char* value = std::getenv(envName);
    if (value == nullptr || std::strlen(value) == 0) {
        return IpAddress::fromString(fallback);
    }
    return IpAddress::fromString(value);
}

} // namespace

int main()
{
    platform::LinuxPlatform platform;

    bool relayOn = false;
    bool relayStateDirty = false;

    const IpAddress gatewayHost = parseIpOrDefault("KNX_INTERFACE_GATEWAY_HOST", "192.168.1.254");
    const NetIpPort gatewayPort(parsePortOrDefault("KNX_INTERFACE_GATEWAY_PORT", 3671));

    auto appResult = startCommissionedProduct(
        platform,
        kInterfaceProduct,
        makeCommissionedBindings(kInterfaceProduct)
            .onCommand<InterfacePort::RelayCommand>([&](bool on) {
                relayOn = on;
                relayStateDirty = true;
                KNX_LOGI("Tp1IpIf", "Relay: %s", relayOn ? "ON" : "OFF");
            })
            .provideState<InterfacePort::RelayState>([&]() {
                return relayOn;
            })
            .onProgrammingModeChanged([](bool enabled) {
                KNX_LOGI("Tp1IpIf", "Programming mode: %s", enabled ? "ON" : "OFF");
            })
            .onLifecycleChanged([](DeviceLifecycleState state) {
                KNX_LOGI("Tp1IpIf", "Lifecycle: %s",
                         state == DeviceLifecycleState::Operational   ? "Operational" :
                         state == DeviceLifecycleState::Commissioning ? "Commissioning" :
                                                                       "Uncommissioned");
            }),
        IpTunnelingOptions{
            .host = gatewayHost,
            .port = gatewayPort,
        });

    if (appResult.isError()) {
        KNX_LOGE("Tp1IpIf", "Start failed: %d", static_cast<int>(appResult.error()));
        return 1;
    }

    auto app = std::move(appResult.value());

    KNX_LOGI("Tp1IpIf", "Commissioned KNXnet/IP tunneling device started on %u.%u.%u.%u:%u",
             static_cast<unsigned>((gatewayHost.raw >> 24u) & 0xFFu),
             static_cast<unsigned>((gatewayHost.raw >> 16u) & 0xFFu),
             static_cast<unsigned>((gatewayHost.raw >> 8u) & 0xFFu),
             static_cast<unsigned>(gatewayHost.raw & 0xFFu),
             static_cast<unsigned>(gatewayPort.value()));

    for (;;) {
        app->loop();

        if (relayStateDirty && app->lifecycleState() == DeviceLifecycleState::Operational) {
            relayStateDirty = false;
            const auto publish = app->publish<InterfacePort::RelayState>(relayOn);
            if (publish.isError()) {
                KNX_LOGW("Tp1IpIf", "State publish failed: %d", static_cast<int>(publish.error()));
            }
        }

        platform.delay(5);
    }
}
