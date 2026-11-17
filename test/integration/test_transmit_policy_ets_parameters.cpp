// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transmit_policy_ets_parameters.cpp
 * @brief End-to-end: ETS parameters → GroupObject transmit policy.
 *
 * The path a product author uses to make send-on-change and cyclic sending
 * configurable from ETS:
 *
 *   parameters()  →  one function that builds the policy  →  setTransmitPolicy<Port>
 *
 * The function runs once after start and again after onParametersChanged. The
 * parameter image is replaced with expert::loadParameterImage, which takes
 * the path a completed ETS download takes, and the resulting policy is read
 * back with transmitPolicy<Port>().
 *
 * CTest label: "integration"
 */

#include "unity.h"

#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/product/commissioned_product.hpp"
#include "knx/product/commissioned_product_expert.hpp"
#include "knx/product/endpoint_policies.hpp"

#include <array>
#include <filesystem>
#include <memory>
#include "knx/objects/object_persistence.hpp"

using namespace knx;
using namespace knx::product;

// ---------------------------------------------------------------------------
// Product: one transmitting temperature port and the parameters that
// configure its transmit policy.
// ---------------------------------------------------------------------------

enum class TpPort : uint16_t {
    Temperature = 0,
};

enum class TpParam : uint8_t {
    // Declaration order is the order in the image; 16-bit values are big-endian.
    ChangeThreshold,  // Dpt9Float, K   → 2 octets
    CyclicSeconds,    // uint16, s      → 2 octets
};

inline constexpr auto kTpBlock = params::parameterBlock<TpParam>(
    {.key = "sending", .name = "Sending"},
    params::param<TpParam::ChangeThreshold, Dpt9Float>(
        {.name = "Change to send", .def = 0.0f, .min = 0, .max = 10, .unit = "K"}),
    params::param<TpParam::CyclicSeconds, uint16_t>(
        {.name = "Send cyclically every", .def = 0, .min = 0, .max = 3600, .unit = "s"}));

inline constexpr auto kTpParameters = params::parameterLayout(kTpBlock);

constexpr auto kTpProduct = makeCommissionedProduct<kTpParameters>(
    makeEndpointDefinition<
        TpPort,
        semantics::TemperatureState<TpPort::Temperature, "temperature", "Temperature">>(
        ProductIdentity{
            .productKey         = "tp_policy",
            .productDisplayName = "Transmit Policy Test",
            .manufacturerId     = ManufacturerId(0x00FA),
            .medium             = endpoint::Medium::TP1,
            .applicationNumber  = 77,
            .applicationVersion = 1,
            .firmwareRevision   = 1,
            .maxApduLength      = knx::config::MAX_APDU_LENGTH,
        },
        PersistencePolicy{
            .namespacePrefix = "tp_policy",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }));

static std::unique_ptr<physical::Tp1MediumBackend> makeNullBackend()
{
    return std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend());
}

static void clearPersistence()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("tp_policy"), ec);
}

// The one place that turns parameters into a policy.
static application::GroupObjectTransmitPolicy policyFrom(params::ViewFor<kTpParameters> parameters)
{
    const auto sending = parameters.block<TpParam>();
    const float threshold = sending.get<TpParam::ChangeThreshold>();
    application::GroupObjectTransmitPolicy policy{};
    policy.onChangeEnabled = threshold > 0.0f;
    policy.changeThreshold = threshold;
    policy.cyclicIntervalMs = endpoint::secondsToMs(sending.get<TpParam::CyclicSeconds>());
    return policy;
}

void setUp(void)  { clearPersistence(); }
void tearDown(void) {}

void test_ets_parameters_configure_transmit_policy(void)
{
    platform::LinuxPlatform platform;

    // The callback gets the new values; the product applies them from its own
    // context, here straight after the download.
    bool changed = false;
    auto result = startCommissionedProduct(
        platform,
        kTpProduct,
        makeCommissionedBindings(kTpProduct)
            .provideState<TpPort::Temperature>([] { return 20.0f; })
            .onParametersChanged([&](params::ViewFor<kTpParameters>) { changed = true; }),
        makeNullBackend());
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    const auto apply = [&] {
        TEST_ASSERT_TRUE(runtime->setTransmitPolicy<TpPort::Temperature>(policyFrom(runtime->parameters())).isOk());
    };
    apply();

    // Defaults: no threshold, no cycle → every publish is sent.
    {
        auto pol = runtime->transmitPolicy<TpPort::Temperature>();
        TEST_ASSERT_TRUE(pol.isOk());
        TEST_ASSERT_FALSE(pol.value().onChangeEnabled);
        TEST_ASSERT_EQUAL_UINT32(0u, pol.value().cyclicIntervalMs);
    }
    TEST_ASSERT_FALSE(changed);  // not called at start

    // ETS downloads: threshold 0.5 K (DPT 9: 0x0032), cycle 30 s.
    const std::array<uint8_t, 4> image = {0x00, 0x32, 0x00, 0x1E};
    TEST_ASSERT_TRUE(expert::loadParameterImage(*runtime, image));
    TEST_ASSERT_TRUE(changed);
    apply();

    auto pol = runtime->transmitPolicy<TpPort::Temperature>();
    TEST_ASSERT_TRUE(pol.isOk());
    TEST_ASSERT_TRUE(pol.value().onChangeEnabled);
    TEST_ASSERT_TRUE(pol.value().changeThreshold > 0.49 && pol.value().changeThreshold < 0.51);
    TEST_ASSERT_EQUAL_UINT32(30000u, pol.value().cyclicIntervalMs);

    // A second download: 2.0 K (0x00C8), no cycle.
    const std::array<uint8_t, 4> image2 = {0x00, 0xC8, 0x00, 0x00};
    TEST_ASSERT_TRUE(expert::loadParameterImage(*runtime, image2));
    apply();

    auto pol2 = runtime->transmitPolicy<TpPort::Temperature>();
    TEST_ASSERT_TRUE(pol2.isOk());
    TEST_ASSERT_TRUE(pol2.value().changeThreshold > 1.99 && pol2.value().changeThreshold < 2.01);
    TEST_ASSERT_EQUAL_UINT32(0u, pol2.value().cyclicIntervalMs);
}

// An image outside the declared bounds changes nothing.
void test_an_image_outside_the_bounds_is_refused(void)
{
    platform::LinuxPlatform platform;
    auto result = startCommissionedProduct(
        platform,
        kTpProduct,
        makeCommissionedBindings(kTpProduct).provideState<TpPort::Temperature>([] { return 20.0f; }),
        makeNullBackend());
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    const std::array<uint8_t, 4> image = {0x00, 0x32, 0xFF, 0xFF};  // cycle 65535 s, the maximum is 3600
    TEST_ASSERT_FALSE(expert::loadParameterImage(*runtime, image));
    TEST_ASSERT_EQUAL_UINT16(0u, runtime->parameters().block<TpParam>().get<TpParam::CyclicSeconds>());

    const std::array<uint8_t, 3> tooShort = {0x00, 0x32, 0x00};
    TEST_ASSERT_FALSE(expert::loadParameterImage(*runtime, tooShort));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ets_parameters_configure_transmit_policy);
    RUN_TEST(test_an_image_outside_the_bounds_is_refused);
    return UNITY_END();
}
