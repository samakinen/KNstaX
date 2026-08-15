// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNX line coupler example (ETS commissioned path).
 *
 * A coupler is a *device* that also routes. This example is deliberately built
 * with the same `startCommissionedProduct` call as an end device — the only
 * difference is `CouplerOptions` instead of a single medium backend. Endpoints,
 * parameters, ETS export and persistence behave exactly as they do elsewhere.
 *
 * What the stack does for you once you pass `CouplerOptions`:
 *
 *   - routes between the two ports per 03/03/03 §2.4.2.4
 *   - derives the coupler role from the individual address ETS assigns
 *     (x.y.0 → line coupler, x.0.0 → backbone coupler)
 *   - publishes the Router Object and binds `PID_ROUTETABLE_CONTROL` to the
 *     filter table the forwarding path actually reads
 *
 * The one thing firmware still has to do is call `syncRouterRoutingConfig()`
 * after a download — see the loop below.
 *
 * This example runs on the host against null backends so it builds and starts
 * anywhere. On real hardware, replace `makeBackend()` with two genuine TP1
 * backends (`createTp1Backend(...)` from physical_factory.hpp) on separate
 * transceivers — one per line.
 */

#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/product/commissioned_product.hpp"
#include "knx/util/log.hpp"

#include <memory>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

namespace {

constexpr const char* TAG = "LineCoupler";

/**
 * A coupler's own communication objects.
 *
 * A coupler does not have to carry any, but giving it a couple is what proves
 * it is a device and not just a bridge: these are commissioned, published and
 * read exactly as on any other product, from either side of the coupler.
 */
enum class CouplerPort : uint16_t {
    /// Reports whether forwarding is currently enabled.
    RoutingEnabledState = 0,
    /// Lets an installation disable forwarding without a download.
    RoutingEnabledCommand = 1,
};

constexpr auto kCouplerProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<
            CouplerPort,
            semantics::SwitchState<CouplerPort::RoutingEnabledState,
                                   "routing_enabled_state", "Routing Enabled (status)">,
            semantics::SwitchCommand<CouplerPort::RoutingEnabledCommand,
                                     "routing_enabled_command", "Routing Enabled (control)">>(
            ProductIdentity{
                .productKey = "tp1_line_coupler",
                .productDisplayName = "TP1 Line Coupler",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::TP1,
                .applicationNumber = 11,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 254,
            },
            PersistencePolicy{
                .namespacePrefix = "tp1_line_coupler",
                .schemaVersion = 1,
                .persistKnxState = true,
            }));

/// Stand-in for a real transceiver. Swap for `createTp1Backend(pins)` on target.
std::unique_ptr<physical::Tp1MediumBackend> makeBackend()
{
    return std::make_unique<physical::NullTp1MediumBackend>();
}

} // namespace

int main()
{
    platform::LinuxPlatform platform;

    bool routingEnabled = true;
    bool routingStateDirty = false;

    auto appResult = startCommissionedProduct(
        platform,
        kCouplerProduct,
        makeCommissionedBindings(kCouplerProduct)
            .onCommand<CouplerPort::RoutingEnabledCommand>([&](bool enabled) {
                routingEnabled = enabled;
                routingStateDirty = true;
                KNX_LOGI(TAG, "Routing %s by group command",
                         enabled ? "enabled" : "disabled");
            })
            .provideState<CouplerPort::RoutingEnabledState>([&]() { return routingEnabled; })
            .onLifecycleChanged([](DeviceLifecycleState state) {
                KNX_LOGI(TAG, "Lifecycle: %s",
                         state == DeviceLifecycleState::Operational   ? "Operational" :
                         state == DeviceLifecycleState::Commissioning ? "Commissioning" :
                                                                        "Uncommissioned");
            }),
        CouplerOptions{
            // Primary is the upstream side: the main line for a line coupler.
            .primary = makeBackend(),
            // Secondary is the subnetwork below it.
            .secondary = makeBackend(),
        });

    if (appResult.isError()) {
        KNX_LOGE(TAG, "Start failed: %d", static_cast<int>(appResult.error()));
        return 1;
    }

    auto app = std::move(appResult.value());

    auto* coupler = app->coupler();
    if (coupler == nullptr) {
        // Only reachable if the product was started without CouplerOptions.
        KNX_LOGE(TAG, "Started without a coupler");
        return 1;
    }

    // Until ETS assigns an address the role is Repeater, and the coupler passes
    // traffic rather than filtering it. That is deliberate: a coupler that
    // blocked everything before commissioning would cut the installation in
    // half the moment it was plugged in.
    KNX_LOGI(TAG, "Coupler role: %s",
             coupler->role() == network::CouplerRole::BackboneCoupler ? "backbone coupler" :
             coupler->role() == network::CouplerRole::LineCoupler     ? "line coupler" :
                                                                        "repeater (uncommissioned)");

    // Optional: watch what the routing decisions are doing. Filtered means the
    // routing condition said no; dropped means the hop count ran out, which is
    // a topology problem rather than a configuration one.
    coupler->setFrameForwardedCallback([](network::CouplerPort origin) {
        KNX_LOGD(TAG, "forwarded from %s",
                 origin == network::CouplerPort::Primary ? "main" : "sub");
    });
    coupler->setFrameDroppedCallback([](network::CouplerPort origin) {
        KNX_LOGW(TAG, "hop count exhausted on %s — check the topology",
                 origin == network::CouplerPort::Primary ? "main" : "sub");
    });

    auto lastLifecycle = app->lifecycleState();

    for (;;) {
        app->loop();

        // ETS writes PID_MAIN_LCCONFIG and the filter table as part of a
        // download; both land in the Router Object. The filter table is applied
        // live through PID_ROUTETABLE_CONTROL, but the LCCONFIG bytes are plain
        // properties and only reach the forwarding path when synced. Doing it
        // on the return to Operational is the cheapest correct trigger.
        const auto lifecycle = app->lifecycleState();
        if (lifecycle != lastLifecycle) {
            lastLifecycle = lifecycle;
            if (lifecycle == DeviceLifecycleState::Operational) {
                (void)app->syncRouterRoutingConfig();
                KNX_LOGI(TAG, "Coupler configuration applied; role is now %s",
                         coupler->role() == network::CouplerRole::BackboneCoupler
                             ? "backbone coupler"
                             : coupler->role() == network::CouplerRole::LineCoupler
                                   ? "line coupler"
                                   : "repeater");
            }
        }

        coupler->setRoutingEnabled(routingEnabled ? Toggle::Enable : Toggle::Disable);

        if (routingStateDirty && lifecycle == DeviceLifecycleState::Operational) {
            routingStateDirty = false;
            const auto publish = app->publish<CouplerPort::RoutingEnabledState>(routingEnabled);
            if (publish.isError()) {
                KNX_LOGW(TAG, "State publish failed: %d", static_cast<int>(publish.error()));
            }
        }

        platform.delay(5);
    }
}
