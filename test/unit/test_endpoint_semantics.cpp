// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_endpoint_semantics.cpp
 * @brief Contract test for the endpoint semantics helper catalog.
 *
 * These helpers are aliases, so most of their value is that they *compile* into
 * a real product definition with the DPT the KNX standard expects.  A helper
 * that names the wrong datapoint type is invisible in firmware and only shows
 * up as a wrong ObjectSize in ETS, so the DPT assertions below are the point of
 * this file, not decoration.
 */

#include "unity.h"
#include "knx/product/commissioned_product.hpp"

using namespace knx;
using namespace knx::application;
using namespace knx::product;

namespace {

enum class Port : uint16_t {
    ShutterMove = 0,
    ShutterStop,
    ShutterPosition,
    SlatAngle,
    Scene,
    SetpointShift,
    HeatCool,
    Occupancy,
    WindowDoor,
    Alarm,
    Energy,
    Counter,
    Pressure,
    WindSpeed,
    ColorTemperature,
    RgbColor,
    Text,
    ReadOnInitSetpoint,
    UrgentAlarm,
    UrgentReadOnInit,
};

// One definition exercising the whole catalog: if any alias names a DPT that
// does not exist, or a value type its DPT cannot carry, this fails to compile.
constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port,
                           semantics::ShutterMoveCommand<Port::ShutterMove, "shutter_move">,
                           semantics::ShutterStopCommand<Port::ShutterStop, "shutter_stop">,
                           semantics::ShutterPositionState<Port::ShutterPosition, "shutter_pos">,
                           semantics::SlatAngleState<Port::SlatAngle, "slat_angle">,
                           semantics::SceneControlCommand<Port::Scene, "scene">,
                           semantics::SetpointShiftCommand<Port::SetpointShift, "setpoint_shift">,
                           semantics::HeatCoolStateInOut<Port::HeatCool, "heat_cool">,
                           semantics::OccupancyState<Port::Occupancy, "occupancy">,
                           semantics::WindowDoorState<Port::WindowDoor, "window">,
                           semantics::AlarmState<Port::Alarm, "alarm">,
                           semantics::EnergyState<Port::Energy, "energy">,
                           semantics::CounterState<Port::Counter, "counter">,
                           semantics::PressureState<Port::Pressure, "pressure">,
                           semantics::WindSpeedState<Port::WindSpeed, "wind">,
                           semantics::ColorTemperatureState<Port::ColorTemperature, "cct">,
                           semantics::RgbColorState<Port::RgbColor, "rgb">,
                           semantics::TextState<Port::Text, "label">,
                           // Port modifiers: read-on-init and priority are
                           // trailing PortSpec defaults, so these transformers
                           // are the only ergonomic way to reach them.
                           semantics::ReadOnInit<
                               semantics::TemperatureStateInOut<Port::ReadOnInitSetpoint, "setpoint">>,
                           semantics::WithPriority<
                               semantics::AlarmState<Port::UrgentAlarm, "alarm_urgent">,
                               Priority::Urgent>,
                           // ...and they compose.
                           semantics::WithPriority<
                               semantics::ReadOnInit<
                                   semantics::SwitchStateInOut<Port::UrgentReadOnInit, "both">>,
                               Priority::Urgent>>(
        ProductIdentity{
            .productKey = "semantics_contract",
            .productDisplayName = "Semantics Contract",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = endpoint::Medium::TP1,
            .applicationNumber = 1,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "semantics_contract",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

constexpr auto kCompiled = compileEndpointDefinition(kProduct.endpointDefinition);

/// Full runtime descriptor generated for `port`.
constexpr auto descriptorFor(Port port) {
    for (const auto& object : kCompiled.runtime.communicationObjects) {
        if (object.logicalId == port) {
            return object;
        }
    }
    return kCompiled.runtime.communicationObjects[0];
}

/// DPT of the communication object generated for `port`.
constexpr DptId dptFor(Port port) {
    for (const auto& object : kCompiled.runtime.communicationObjects) {
        if (object.logicalId == port) {
            return object.dpt;
        }
    }
    return DptId{};
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_shutter_helpers_use_standard_dpts(void) {
    // DPT 1.008 Up/Down and 1.007 Step are what visualisations key off; plain
    // DPT 1.001 would work on the wire but show up as a switch in ETS.
    TEST_ASSERT_TRUE(dptFor(Port::ShutterMove) == dptids::UpDown);
    TEST_ASSERT_TRUE(dptFor(Port::ShutterStop) == dptids::Step);
    TEST_ASSERT_TRUE(dptFor(Port::ShutterPosition) == dptids::Percent);
    TEST_ASSERT_TRUE(dptFor(Port::SlatAngle) == dptids::Percent);
}

void test_scene_helper_uses_scene_control(void) {
    // DPT 18.001 carries the learn bit; 17.001 does not.  Recall+store needs 18.
    TEST_ASSERT_TRUE(dptFor(Port::Scene) == dptids::SceneControl);
}

void test_hvac_helpers_use_standard_dpts(void) {
    // Setpoint shift is DPT 6.010 counter pulses, not a Kelvin float.
    TEST_ASSERT_TRUE(dptFor(Port::SetpointShift) == dptids::CounterPulsesSigned);
    TEST_ASSERT_TRUE(dptFor(Port::HeatCool) == dptids::HeatCool);
}

void test_binary_input_helpers_use_distinct_subtypes(void) {
    // These three are all 1-bit but must not collapse onto DPT 1.001: the
    // subtype is what tells ETS whether "1" means occupied, open, or alarm.
    TEST_ASSERT_TRUE(dptFor(Port::Occupancy) == dptids::Occupancy);
    TEST_ASSERT_TRUE(dptFor(Port::WindowDoor) == dptids::WindowDoor);
    TEST_ASSERT_TRUE(dptFor(Port::Alarm) == dptids::Alarm);
}

void test_metering_and_environment_helpers(void) {
    TEST_ASSERT_TRUE(dptFor(Port::Energy) == dptids::Energy);
    TEST_ASSERT_TRUE(dptFor(Port::Counter) == dptids::Counter16);
    TEST_ASSERT_TRUE(dptFor(Port::Pressure) == dptids::Pressure);
    TEST_ASSERT_TRUE(dptFor(Port::WindSpeed) == dptids::WindSpeed);
}

void test_lighting_and_text_helpers(void) {
    TEST_ASSERT_TRUE(dptFor(Port::ColorTemperature) == dptids::ColorTemperature);
    TEST_ASSERT_TRUE(dptFor(Port::RgbColor) == dptids::RgbColor);
    TEST_ASSERT_TRUE(dptFor(Port::Text) == dptids::Label);
}

void test_new_catalog_dpts_have_known_widths(void) {
    // dptPayloadBits() feeds the Group Object Descriptor's Value Field Type, so
    // a DPT added to the catalog without a known width would serialise a
    // descriptor ETS cannot parse.
    TEST_ASSERT_EQUAL(1u, dptPayloadBits(dptids::Trigger));
    TEST_ASSERT_EQUAL(8u, dptPayloadBits(dptids::CounterPulsesSigned));
    TEST_ASSERT_EQUAL(16u, dptPayloadBits(dptids::WindSpeed));
    TEST_ASSERT_EQUAL(16u, dptPayloadBits(dptids::ColorTemperature));
    TEST_ASSERT_EQUAL(24u, dptPayloadBits(dptids::RgbColor));
}

void test_new_catalog_dpts_round_trip(void) {
    // The codec dispatch is keyed on DptValue::Type rather than a per-entry
    // function pointer, so a newly added catalog entry must still find its
    // codec.  These are the entries added alongside the semantics catalog.
    uint8_t buffer[8]{};

    const auto encodedShift = encodeDptValue(dptids::CounterPulsesSigned,
                                             DptValue(static_cast<int8_t>(-7)), buffer);
    TEST_ASSERT_TRUE(encodedShift.isOk());
    const auto decodedShift = decodeDptValue(dptids::CounterPulsesSigned,
                                             std::span<const uint8_t>(buffer, encodedShift.value()));
    TEST_ASSERT_TRUE(decodedShift.isOk());
    TEST_ASSERT_EQUAL(-7, *decodedShift.value().tryGet<int8_t>());

    const auto encodedWind = encodeDptValue(dptids::WindSpeed, DptValue(12.5f), buffer);
    TEST_ASSERT_TRUE(encodedWind.isOk());
    const auto decodedWind = decodeDptValue(dptids::WindSpeed,
                                            std::span<const uint8_t>(buffer, encodedWind.value()));
    TEST_ASSERT_TRUE(decodedWind.isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 12.5f, *decodedWind.value().tryGet<float>());
}

// --- Port modifiers --------------------------------------------------------

void test_read_on_init_modifier_reaches_the_runtime_descriptor(void) {
    // Without this the read-on-init machinery in the BAU is unreachable: no
    // semantic alias sets the flag, and hand-writing a 13-parameter PortSpec is
    // not a realistic authoring path.
    TEST_ASSERT_TRUE(descriptorFor(Port::ReadOnInitSetpoint).readOnInit);
    // The DPT and direction of the wrapped port must survive the transform.
    TEST_ASSERT_TRUE(dptFor(Port::ReadOnInitSetpoint) == dptids::Temperature);

    // Ports that did not ask for it stay off — the spec warns against enabling
    // read-on-init by default.
    TEST_ASSERT_FALSE(descriptorFor(Port::Alarm).readOnInit);
    TEST_ASSERT_FALSE(descriptorFor(Port::ShutterMove).readOnInit);
}

void test_priority_modifier_reaches_the_runtime_descriptor(void) {
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::Urgent),
                      static_cast<int>(descriptorFor(Port::UrgentAlarm).priority));
    TEST_ASSERT_TRUE(dptFor(Port::UrgentAlarm) == dptids::Alarm);

    // Everything else keeps the Low default; a device that sent everything at
    // Urgent would degrade the whole line.
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::Low),
                      static_cast<int>(descriptorFor(Port::Alarm).priority));
}

void test_modifiers_compose(void) {
    const auto descriptor = descriptorFor(Port::UrgentReadOnInit);
    TEST_ASSERT_TRUE(descriptor.readOnInit);
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::Urgent), static_cast<int>(descriptor.priority));
    // And the underlying port is untouched: StateInOut is readable and writable.
    TEST_ASSERT_TRUE(descriptor.readable);
    TEST_ASSERT_TRUE(descriptor.writable);
}

void test_read_on_init_reaches_the_ets_export(void) {
    // The runtime flag and the exported ReadOnInitFlag must agree, otherwise
    // ETS shows one configuration and the device performs another.
    constexpr auto exported = kCompiled.exportDescriptor;
    bool found = false;
    for (const auto& object : exported.communicationObjects) {
        if (object.logicalId == static_cast<uint16_t>(Port::ReadOnInitSetpoint)) {
            TEST_ASSERT_TRUE(object.readOnInit);
            found = true;
        }
        if (object.logicalId == static_cast<uint16_t>(Port::Alarm)) {
            TEST_ASSERT_FALSE(object.readOnInit);
        }
    }
    TEST_ASSERT_TRUE(found);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_read_on_init_modifier_reaches_the_runtime_descriptor);
    RUN_TEST(test_priority_modifier_reaches_the_runtime_descriptor);
    RUN_TEST(test_modifiers_compose);
    RUN_TEST(test_read_on_init_reaches_the_ets_export);
    RUN_TEST(test_shutter_helpers_use_standard_dpts);
    RUN_TEST(test_scene_helper_uses_scene_control);
    RUN_TEST(test_hvac_helpers_use_standard_dpts);
    RUN_TEST(test_binary_input_helpers_use_distinct_subtypes);
    RUN_TEST(test_metering_and_environment_helpers);
    RUN_TEST(test_lighting_and_text_helpers);
    RUN_TEST(test_new_catalog_dpts_have_known_widths);
    RUN_TEST(test_new_catalog_dpts_round_trip);
    return UNITY_END();
}
