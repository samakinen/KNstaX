// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNXnet/IP Routing Device example (ETS commissioned path).
 *
 * An **end device** that reaches the bus over KNXnet/IP routing multicast —
 * not a coupler. "Routing" here is the KNXnet/IP transport mode, not the act
 * of forwarding between two subnetworks. For a device that couples two lines,
 * see `examples/tp1_line_coupler`.
 *
 * It demonstrates commissioned startup over KNXnet/IP routing via:
 *
 *   startCommissionedProduct(...) + IpRoutingOptions
 *
 * Firmware does not own KNX commissioning plumbing. ETS assigns the individual
 * address and group addresses, and KNstaX restores them from persistence.
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

enum class RouterPort : uint16_t {
    RelayCommand = 0,
    RelayState = 1,
};

constexpr auto kRoutingProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<
            RouterPort,
            semantics::SwitchCommand<RouterPort::RelayCommand, "relay_command", "Relay Command">,
            semantics::SwitchState<RouterPort::RelayState, "relay_state", "Relay State">>(
            ProductIdentity{
                .productKey = "ip_routing_device",
                .productDisplayName = "KNXnet/IP Routing Device",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::IP_Routing,
                .applicationNumber = 9,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 254,
            },
            PersistencePolicy{
                .namespacePrefix = "ip_routing_device",
                .schemaVersion = 1,
                .persistKnxState = true,
            }));

IpAddress parseIpOrDefault(const char* envName, const char* fallback)
{
    const char* value = std::getenv(envName);
    if (value == nullptr || std::strlen(value) == 0) {
        return IpAddress::fromString(fallback);
    }
    return IpAddress::fromString(value);
}

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

} // namespace

int main()
{
    platform::LinuxPlatform platform;

    bool relayOn = false;
    bool relayStateDirty = false;

    const IpAddress multicastGroup = parseIpOrDefault("KNX_ROUTER_MCAST_GROUP", "224.0.23.12");
    const NetIpPort multicastPort(parsePortOrDefault("KNX_ROUTER_MCAST_PORT", 3671));
    const IpAddress interfaceAddress = parseIpOrDefault("KNX_ROUTER_IFACE", "0.0.0.0");

    auto appResult = startCommissionedProduct(
        platform,
        kRoutingProduct,
        makeCommissionedBindings(kRoutingProduct)
            .onCommand<RouterPort::RelayCommand>([&](bool on) {
                relayOn = on;
                relayStateDirty = true;
                KNX_LOGI("Tp1IpRouter", "Relay: %s", relayOn ? "ON" : "OFF");
            })
            .provideState<RouterPort::RelayState>([&]() {
                return relayOn;
            })
            .onProgrammingModeChanged([](bool enabled) {
                KNX_LOGI("Tp1IpRouter", "Programming mode: %s", enabled ? "ON" : "OFF");
            })
            .onLifecycleChanged([](DeviceLifecycleState state) {
                KNX_LOGI("Tp1IpRouter", "Lifecycle: %s",
                         state == DeviceLifecycleState::Operational   ? "Operational" :
                         state == DeviceLifecycleState::Commissioning ? "Commissioning" :
                                                                       "Uncommissioned");
            }),
        IpRoutingOptions{
            .multicastGroup = multicastGroup,
            .port = multicastPort,
            .interfaceAddress = interfaceAddress,
        });

    if (appResult.isError()) {
        KNX_LOGE("Tp1IpRouter", "Start failed: %d", static_cast<int>(appResult.error()));
        return 1;
    }

    auto app = std::move(appResult.value());

    KNX_LOGI("Tp1IpRouter", "Commissioned KNXnet/IP routing device started (group=%u.%u.%u.%u:%u)",
             static_cast<unsigned>((multicastGroup.raw >> 24u) & 0xFFu),
             static_cast<unsigned>((multicastGroup.raw >> 16u) & 0xFFu),
             static_cast<unsigned>((multicastGroup.raw >> 8u) & 0xFFu),
             static_cast<unsigned>(multicastGroup.raw & 0xFFu),
             static_cast<unsigned>(multicastPort.value()));

    for (;;) {
        app->loop();

        if (relayStateDirty && app->lifecycleState() == DeviceLifecycleState::Operational) {
            relayStateDirty = false;
            const auto publish = app->publish<RouterPort::RelayState>(relayOn);
            if (publish.isError()) {
                KNX_LOGW("Tp1IpRouter", "State publish failed: %d", static_cast<int>(publish.error()));
            }
        }

        platform.delay(5);
    }
}
