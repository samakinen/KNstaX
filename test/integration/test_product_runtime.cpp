// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/product/endpoint.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>
#include "knx/objects/object_persistence.hpp"

using namespace knx;
using namespace knx::application;
using namespace knx::product;

namespace {

std::vector<uint8_t> encodeTp1Frame(const datalink::LDataFrame& frame)
{
    std::array<uint8_t, 32> buffer{};
    const auto encoded = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encoded.isOk());
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value());
}

datalink::LDataFrame decodeTp1Frame(std::span<const uint8_t> raw)
{
    datalink::LDataFrame frame;
    const auto decoded = datalink::FrameCodec::decodeFrame(raw, frame);
    TEST_ASSERT_TRUE(decoded.isOk());
    return frame;
}

template <typename DptTag, typename ValueT>
std::vector<uint8_t> encodePayload(ValueT value)
{
    std::array<uint8_t, 16> buffer{};
    const auto encoded = application::DptTraits<DptTag>::encode(value, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encoded.isOk());
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value());
}

template <typename RuntimeT>
void pumpRuntime(RuntimeT& runtime, int iterations = 20)
{
    for (int index = 0; index < iterations; ++index) {
        runtime.loop();
    }
}

// Inbound frames are delivered on the data link RX task thread, so pump the
// runtime until the observable side effect appears (bounded wait).
template <typename RuntimeT>
bool pumpRuntimeUntil(RuntimeT& runtime, const std::function<bool()>& done, uint32_t timeoutMs = 500)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        runtime.loop();
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return done();
}

enum class SwitchPort : uint16_t {
    RelayCommand = 0,
    RelayState = 1,
};

using RelayCommandPort = CommandPort<SwitchPort::RelayCommand,
                                     bool,
                                     "relay_command",
                                     "Relay command",
                                     dptids::Switch>;
using RelayStatePort = StatePort<SwitchPort::RelayState,
                                 bool,
                                 "relay_state",
                                 "Relay state",
                                 dptids::Switch,
                                 true>;

constexpr auto kSwitchDefinition =
    makeEndpointDefinition<SwitchPort, RelayCommandPort, RelayStatePort>(
        ProductIdentity{
            .productKey = "tp1_switch",
            .productDisplayName = "TP1 Switch",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 1,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp1_switch",
            .schemaVersion = 1,
            .persistKnxState = true,
        });

enum class ThermostatPort : uint16_t {
    Setpoint = 0,
    MeasuredTemperature = 1,
    HvacMode = 2,
};

using SetpointPort = StateInOutPort<ThermostatPort::Setpoint,
                                    float,
                                    "setpoint_temperature",
                                    "Setpoint temperature",
                                    dptids::Temperature,
                                    true>;
using MeasuredTemperaturePort = StatePort<ThermostatPort::MeasuredTemperature,
                                          float,
                                          "measured_temperature",
                                          "Measured temperature",
                                          dptids::Temperature,
                                          false>;
using HvacModePort = StateInOutPort<ThermostatPort::HvacMode,
                                    Dpt20Mode,
                                    "hvac_mode",
                                    "HVAC mode",
                                    dptids::HvacMode,
                                    true>;

constexpr auto kThermostatDefinition =
    makeEndpointDefinition<ThermostatPort, SetpointPort, MeasuredTemperaturePort, HvacModePort>(
        ProductIdentity{
            .productKey = "tp1_thermostat",
            .productDisplayName = "TP1 Thermostat",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 2,
            .applicationVersion = 1,
            .firmwareRevision = 3,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp1_thermostat",
            .schemaVersion = 2,
            .persistKnxState = true,
        });

std::unique_ptr<platform::LinuxPlatform> gPlatform;

void clearPersistenceStore()
{
    std::error_code error;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), error);
}

} // namespace

void setUp(void)
{
    clearPersistenceStore();
    gPlatform = std::make_unique<platform::LinuxPlatform>();
}

void tearDown(void)
{
    gPlatform.reset();
}

void test_product_runtime_switch_runs_through_started_runtime(void)
{
    bool relayState = false;
    bool programmingMode = false;

    auto bindings = EndpointBindings<decltype(kSwitchDefinition)>{}
                        .onCommand<SwitchPort::RelayCommand>([&](bool on) {
                            relayState = on;
                        })
                        .provideState<SwitchPort::RelayState>([&]() {
                            return relayState;
                        })
                        .onProgrammingModeChanged([&](bool enabled) {
                            programmingMode = enabled;
                        });

    auto physical = std::make_unique<test::MockPhysicalLayer>();
    auto* physicalRaw = physical.get();
    auto stackPort = test::createTp1TestStackPort(*gPlatform, std::move(physical));

    EndpointRuntime<decltype(kSwitchDefinition)> runtime(
        kSwitchDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 40),
            .persistenceNamespace = "switch_runtime_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_TRUE(runtime.bindGroupAddress<SwitchPort::RelayCommand>(GroupAddress(2, 0, 1)).isOk());
    TEST_ASSERT_TRUE(runtime.bindGroupAddress<SwitchPort::RelayState>(GroupAddress(2, 0, 2)).isOk());
    TEST_ASSERT_TRUE(runtime.start(*gPlatform, std::move(stackPort)).isOk());

    TEST_ASSERT_TRUE(runtime.publish<SwitchPort::RelayState>(true).isOk());
    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalRaw->getSentFrame(sentFrame));
    const auto published = decodeTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(2, 0, 2).value(), published.destination.value());
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(APCIService::GroupValueWrite),
                           static_cast<unsigned int>(published.apci().service()));
    TEST_ASSERT_EQUAL_UINT8(1u, published.apci().data6());

    datalink::LDataFrame inboundWrite;
    inboundWrite.source = IndividualAddress(1, 1, 5);
    inboundWrite.destination = GroupAddress(2, 0, 1);
    inboundWrite.destinationType = AddressType::Group;
    inboundWrite.ackRequested = false;
    inboundWrite.setTpdu(protocol::TPCI::UnnumberedData,
                         application::APCIField::create(APCIService::GroupValueWrite, 0x01),
                         {});
    const auto inboundWriteRaw = encodeTp1Frame(inboundWrite);
    physicalRaw->injectFrame(inboundWriteRaw);
    TEST_ASSERT_TRUE(pumpRuntimeUntil(runtime, [&]() { return relayState; }));

    runtime.toggleProgrammingMode();
    pumpRuntime(runtime, 50);
    TEST_ASSERT_TRUE(runtime.isProgrammingModeActive());
    TEST_ASSERT_TRUE(programmingMode);

    runtime.stop();
}

void test_product_runtime_thermostat_derives_read_response_and_state_updates_when_started(void)
{
    float setpoint = 20.0f;
    float measuredTemperature = 21.5f;
    Dpt20Mode hvacMode = Dpt20Mode::Auto;

    auto bindings = EndpointBindings<decltype(kThermostatDefinition)>{}
                        .onStateWrite<ThermostatPort::Setpoint>([&](float value) {
                            setpoint = value;
                        })
                        .provideState<ThermostatPort::MeasuredTemperature>([&]() {
                            return measuredTemperature;
                        })
                        .onCommand<ThermostatPort::HvacMode>([&](Dpt20Mode value) {
                            hvacMode = value;
                        });

    auto physical = std::make_unique<test::MockPhysicalLayer>();
    auto* physicalRaw = physical.get();
    auto stackPort = test::createTp1TestStackPort(*gPlatform, std::move(physical));

    EndpointRuntime<decltype(kThermostatDefinition)> runtime(
        kThermostatDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 41),
            .persistenceNamespace = "thermostat_runtime_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_TRUE(runtime.bindGroupAddress<ThermostatPort::Setpoint>(GroupAddress(3, 0, 1)).isOk());
    TEST_ASSERT_TRUE(runtime.bindGroupAddress<ThermostatPort::MeasuredTemperature>(GroupAddress(3, 0, 2)).isOk());
    TEST_ASSERT_TRUE(runtime.bindGroupAddress<ThermostatPort::HvacMode>(GroupAddress(3, 0, 3)).isOk());
    TEST_ASSERT_TRUE(runtime.start(*gPlatform, std::move(stackPort)).isOk());

    datalink::LDataFrame inboundSetpoint;
    inboundSetpoint.source = IndividualAddress(1, 1, 6);
    inboundSetpoint.destination = GroupAddress(3, 0, 1);
    inboundSetpoint.destinationType = AddressType::Group;
    inboundSetpoint.ackRequested = false;
    const auto setpointPayload = encodePayload<application::dpttags::Temperature>(23.5f);
    inboundSetpoint.setTpdu(protocol::TPCI::UnnumberedData,
                            APCIService::GroupValueWrite,
                            std::span<const uint8_t>(setpointPayload));
    const auto inboundSetpointRaw = encodeTp1Frame(inboundSetpoint);
    physicalRaw->injectFrame(inboundSetpointRaw);
    TEST_ASSERT_TRUE(pumpRuntimeUntil(runtime, [&]() { return std::fabs(setpoint - 23.5f) < 0.2f; }));

    datalink::LDataFrame inboundHvac;
    inboundHvac.source = IndividualAddress(1, 1, 7);
    inboundHvac.destination = GroupAddress(3, 0, 3);
    inboundHvac.destinationType = AddressType::Group;
    inboundHvac.ackRequested = false;
    const auto hvacPayload = encodePayload<application::dpttags::HvacMode>(Dpt20Mode::Comfort);
    inboundHvac.setTpdu(protocol::TPCI::UnnumberedData,
                        APCIService::GroupValueWrite,
                        std::span<const uint8_t>(hvacPayload));
    const auto inboundHvacRaw = encodeTp1Frame(inboundHvac);
    physicalRaw->injectFrame(inboundHvacRaw);
    TEST_ASSERT_TRUE(pumpRuntimeUntil(runtime, [&]() { return hvacMode == Dpt20Mode::Comfort; }));

    datalink::LDataFrame inboundRead;
    inboundRead.source = IndividualAddress(1, 1, 8);
    inboundRead.destination = GroupAddress(3, 0, 2);
    inboundRead.destinationType = AddressType::Group;
    inboundRead.ackRequested = false;
    inboundRead.setTpdu(protocol::TPCI::UnnumberedData, APCIService::GroupValueRead, {});
    const auto inboundReadRaw = encodeTp1Frame(inboundRead);
    physicalRaw->injectFrame(inboundReadRaw);

    std::vector<uint8_t> responseFrame;
    TEST_ASSERT_TRUE(pumpRuntimeUntil(runtime, [&]() { return physicalRaw->tryGetSentFrame(responseFrame); }));
    const auto response = decodeTp1Frame(responseFrame);
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(3, 0, 2).value(), response.destination.value());
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(APCIService::GroupValueResponse),
                           static_cast<unsigned int>(response.apci().service()));
    TEST_ASSERT_FALSE(response.payload().empty());

    const auto decodedTemperature = application::DptTraits<application::dpttags::Temperature>::decode(response.payload());
    TEST_ASSERT_TRUE(decodedTemperature.isOk());
    TEST_ASSERT_TRUE(std::fabs(decodedTemperature.value() - measuredTemperature) < 0.2f);

    runtime.stop();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_runtime_switch_runs_through_started_runtime);
    RUN_TEST(test_product_runtime_thermostat_derives_read_response_and_state_updates_when_started);
    return UNITY_END();
}