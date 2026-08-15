// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transmit_policy_ets_parameters.cpp
 * @brief End-to-end: ETS parameters → GroupObject transmit policy.
 *
 * Verifies the reusable path a product author uses to make send-on-change /
 * cyclic / min-interval configurable from ETS:
 *   product parameters  →  onParameterChanged  →  TransmitPolicyBinder
 *                       →  CommissionedProductRuntime::setTransmitPolicy<Port>
 *
 * ETS writes the parameter block over the KNX management model (exercised here
 * via expert::applyParameterDataBytes, the same ApplicationProgramObject →
 * ParameterState::applyFromBytes chain a real download uses), and the resulting
 * policy is read back with transmitPolicy<Port>().
 *
 * Also unit-tests TransmitPolicyBinder aggregation/apply-on-change in isolation.
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
// Product: one transmitting temperature port + two ETS parameters that
// configure its transmit policy (change threshold + cyclic heartbeat).
// ---------------------------------------------------------------------------

enum class TpPort : uint16_t {
    Temperature = 0,
};

enum class TpParam : uint16_t {
    // Declaration order defines the ProgramData byte layout (big-endian):
    ChangeThresholdCenti = 0,  // uint16, 0.01 °C units  → 2 bytes
    CyclicSeconds        = 1,  // uint16, seconds         → 2 bytes
};

constexpr auto kTpProduct = makeCommissionedProduct(
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
            .maxApduLength      = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp_policy",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }),
    makeParameterSchema(
        parameter<TpParam::ChangeThresholdCenti>("change_threshold_centi", uint16_t{0}),
        parameter<TpParam::CyclicSeconds>("cyclic_seconds", uint16_t{0})));

static std::unique_ptr<physical::Tp1MediumBackend> makeNullBackend()
{
    return std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend());
}

static void clearPersistence()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), ec);
}

// The binder is owner-managed and long-lived so the onParameterChanged closures
// can capture it safely (they are bound before the runtime exists).
static endpoint::TransmitPolicyBinder g_tempPolicy;

void setUp(void)  { clearPersistence(); g_tempPolicy = endpoint::TransmitPolicyBinder{}; }
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Binder unit test: partial updates aggregate; apply fires with the full policy.
// ---------------------------------------------------------------------------
void test_binder_aggregates_and_applies(void)
{
    endpoint::TransmitPolicyBinder binder;
    int applyCount = 0;
    application::GroupObjectTransmitPolicy last{};
    binder.bindApply([&](const application::GroupObjectTransmitPolicy& p) {
        ++applyCount;
        last = p;
    });
    TEST_ASSERT_EQUAL_INT(1, applyCount);  // bindApply applies the initial (empty) policy

    binder.setChangeThreshold(0.5);        // also flips onChangeEnabled true
    TEST_ASSERT_EQUAL_INT(2, applyCount);
    TEST_ASSERT_TRUE(last.onChangeEnabled);
    TEST_ASSERT_TRUE(last.changeThreshold > 0.49 && last.changeThreshold < 0.51);

    binder.setCyclicIntervalMs(endpoint::secondsToMs(30));
    TEST_ASSERT_EQUAL_INT(3, applyCount);
    TEST_ASSERT_EQUAL_UINT32(30000u, last.cyclicIntervalMs);
    TEST_ASSERT_TRUE(last.onChangeEnabled);          // earlier field preserved
    TEST_ASSERT_TRUE(last.changeThreshold > 0.49);
}

// ---------------------------------------------------------------------------
// Full round-trip: ETS parameter block → transmit policy on the port.
// ---------------------------------------------------------------------------
void test_ets_parameters_configure_transmit_policy(void)
{
    platform::LinuxPlatform platform;

    auto result = startCommissionedProduct(
        platform,
        kTpProduct,
        makeCommissionedBindings(kTpProduct)
            .provideState<TpPort::Temperature>([] { return 20.0f; })
            .onParameterChanged<TpParam::ChangeThresholdCenti>([&](uint16_t centi) {
                g_tempPolicy.setChangeThreshold(static_cast<double>(centi) / 100.0);
            })
            .onParameterChanged<TpParam::CyclicSeconds>([&](uint16_t seconds) {
                g_tempPolicy.setCyclicIntervalMs(endpoint::secondsToMs(seconds));
            }),
        makeNullBackend());
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    // Wire the apply function now that the runtime exists.
    g_tempPolicy.bindApply([&](const application::GroupObjectTransmitPolicy& p) {
        (void)runtime->setTransmitPolicy<TpPort::Temperature>(p);
    });

    // Defaults: parameters are 0 → policy inert (send every publish).
    {
        auto pol = runtime->transmitPolicy<TpPort::Temperature>();
        TEST_ASSERT_TRUE(pol.isOk());
        TEST_ASSERT_FALSE(pol.value().onChangeEnabled);
        TEST_ASSERT_EQUAL_UINT32(0u, pol.value().cyclicIntervalMs);
    }

    // Simulate ETS writing the parameter block: threshold = 50 (0.50 °C),
    // cyclic = 30 s. Layout is big-endian in declaration order (2 + 2 bytes).
    const std::array<uint8_t, 4> etsBytes = {
        0x00, 0x32,  // ChangeThresholdCenti = 50
        0x00, 0x1E,  // CyclicSeconds = 30
    };
    expert::applyParameterDataBytes(*runtime, etsBytes);

    // The policy is now configured on the transmitting port's group object.
    auto pol = runtime->transmitPolicy<TpPort::Temperature>();
    TEST_ASSERT_TRUE(pol.isOk());
    TEST_ASSERT_TRUE(pol.value().onChangeEnabled);
    TEST_ASSERT_TRUE(pol.value().changeThreshold > 0.49 && pol.value().changeThreshold < 0.51);
    TEST_ASSERT_EQUAL_UINT32(30000u, pol.value().cyclicIntervalMs);

    // A subsequent ETS re-parameterisation (larger threshold, no cyclic) is
    // applied live through the same binder path.
    const std::array<uint8_t, 4> etsBytes2 = {
        0x00, 0xC8,  // ChangeThresholdCenti = 200 → 2.00 °C
        0x00, 0x00,  // CyclicSeconds = 0 (heartbeat off)
    };
    expert::applyParameterDataBytes(*runtime, etsBytes2);

    auto pol2 = runtime->transmitPolicy<TpPort::Temperature>();
    TEST_ASSERT_TRUE(pol2.isOk());
    TEST_ASSERT_TRUE(pol2.value().changeThreshold > 1.99 && pol2.value().changeThreshold < 2.01);
    TEST_ASSERT_EQUAL_UINT32(0u, pol2.value().cyclicIntervalMs);
}

// ---------------------------------------------------------------------------
// The export descriptor carries the two parameters, so ETS shows them.
// ---------------------------------------------------------------------------
void test_export_descriptor_includes_parameters(void)
{
    constexpr auto exportDesc = makeCommissionedExportDescriptor(kTpProduct);
    static_assert(exportDesc.kParameterCount == 2u,
                  "transmit-policy parameters must appear in the export descriptor");
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(exportDesc.kParameterCount));
}

// ---------------------------------------------------------------------------
// DPT9 half-float parameters: ETS writes 2-byte DPT9 values into the
// parameter block; a mixed DPT9 + Unsigned16 schema checks both the decode
// and the byte-offset math.
// ---------------------------------------------------------------------------

enum class D9Param : uint16_t {
    SetpointC = 0,   // Dpt9Float → 2 bytes at offset 0
    IntervalS = 1,   // uint16    → 2 bytes at offset 2
};

constexpr auto kD9Product = makeCommissionedProduct(
    makeEndpointDefinition<
        TpPort,
        semantics::TemperatureState<TpPort::Temperature, "temperature", "Temperature">>(
        ProductIdentity{
            .productKey         = "tp_dpt9",
            .productDisplayName = "DPT9 Parameter Test",
            .manufacturerId     = ManufacturerId(0x00FA),
            .medium             = endpoint::Medium::TP1,
            .applicationNumber  = 78,
            .applicationVersion = 1,
            .firmwareRevision   = 1,
            .maxApduLength      = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp_dpt9",
            .schemaVersion   = 1,
            .persistKnxState = true,
        }),
    makeParameterSchema(
        parameter<D9Param::SetpointC>("setpoint", "Setpoint (°C)", Dpt9Float{22.0f}),
        parameter<D9Param::IntervalS>("interval", "Interval (s)", uint16_t{60})));

void test_dpt9_parameter_round_trip(void)
{
    // Export kind: 2-byte FloatDpt9 followed by a 2-byte Unsigned16.
    constexpr auto exportDesc = makeCommissionedExportDescriptor(kD9Product);
    static_assert(exportDesc.kParameterCount == 2u);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ExportParameterValueKind::FloatDpt9),
                          static_cast<int>(exportDesc.parameters[0].valueKind));
    TEST_ASSERT_TRUE(exportDesc.parameters[0].defaultValue > 21.9 && exportDesc.parameters[0].defaultValue < 22.1);

    platform::LinuxPlatform platform;
    float seenSetpoint = 0.0f;
    uint16_t seenInterval = 0;

    auto result = startCommissionedProduct(
        platform,
        kD9Product,
        makeCommissionedBindings(kD9Product)
            .provideState<TpPort::Temperature>([] { return 20.0f; })
            .onParameterChanged<D9Param::SetpointC>([&](Dpt9Float v) { seenSetpoint = v; })
            .onParameterChanged<D9Param::IntervalS>([&](uint16_t v) { seenInterval = v; }),
        makeNullBackend());
    TEST_ASSERT_TRUE(result.isOk());
    auto runtime = std::move(result.value());

    // Known DPT9 wire vector: 21.5 °C = 0x0C 0x33
    // (E=1, M=1075: high byte = sign 0 | EEEE 0001 | MMM 100, low byte 0x33).
    // Followed by interval = 300 s (0x01 0x2C) at offset 2.
    const std::array<uint8_t, 4> etsBytes = {0x0C, 0x33, 0x01, 0x2C};
    expert::applyParameterDataBytes(*runtime, etsBytes);

    TEST_ASSERT_TRUE(seenSetpoint > 21.49f && seenSetpoint < 21.51f);
    TEST_ASSERT_EQUAL_UINT16(300, seenInterval);

    const float stored = runtime->parameters().get<D9Param::SetpointC>();
    TEST_ASSERT_TRUE(stored > 21.49f && stored < 21.51f);

    // Serialisation round-trip: the persisted block re-encodes the DPT9 value
    // to the same wire bytes (applyCommissionedParameter persists via toBytes).
    (void)runtime->applyCommissionedParameter<D9Param::SetpointC>(Dpt9Float{21.5f});
    const float reread = runtime->parameters().get<D9Param::SetpointC>();
    TEST_ASSERT_TRUE(reread > 21.49f && reread < 21.51f);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_binder_aggregates_and_applies);
    RUN_TEST(test_ets_parameters_configure_transmit_policy);
    RUN_TEST(test_export_descriptor_includes_parameters);
    RUN_TEST(test_dpt9_parameter_round_trip);
    return UNITY_END();
}
