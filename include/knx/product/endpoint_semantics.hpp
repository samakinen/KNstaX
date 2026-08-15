// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_semantics.hpp
 * @brief Canonical composable KNX signal helpers for endpoint authoring.
 */

#pragma once

#include "knx/product/endpoint_definition.hpp"

namespace knx::product::endpoint::semantics {

using knx::product::FixedString;
using knx::product::endpoint::CommandPort;
using knx::product::endpoint::StatePort;
using knx::product::endpoint::StateInOutPort;

// Port modifiers — see endpoint_definition.hpp. Re-exported here so a product
// can reach them through the same `semantics::` namespace as the port helpers
// they wrap.
using knx::product::endpoint::ReadOnInit;
using knx::product::endpoint::WithPriority;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using SwitchCommand =
    CommandPort<LogicalId, bool, Key, DisplayName, application::dptids::Switch, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using SwitchState =
    StatePort<LogicalId, bool, Key, DisplayName, application::dptids::Switch, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using SwitchStateInOut =
    StateInOutPort<LogicalId, bool, Key, DisplayName, application::dptids::Switch, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using PercentCommand =
    CommandPort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using PercentState =
    StatePort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using RelativeDimmingCommand = CommandPort<LogicalId,
                                           application::Dpt3Value,
                                           Key,
                                           DisplayName,
                                           application::dptids::Dimming,
                                           Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using TemperatureState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::Temperature, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using TemperatureStateInOut = StateInOutPort<LogicalId,
                                             float,
                                             Key,
                                             DisplayName,
                                             application::dptids::Temperature,
                                             Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using HvacModeStateInOut = StateInOutPort<LogicalId,
                                          application::Dpt20Mode,
                                          Key,
                                          DisplayName,
                                          application::dptids::HvacModeComfort,
                                          Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using HumidityState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::Humidity, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using Co2State =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::CO2, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using IlluminanceState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::Illuminance, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Shutter / blind
//
// A KNX shutter channel is conventionally four objects: a long-press Up/Down
// move, a short-press Stop (which doubles as slat step), and absolute position
// feedback for the curtain and the slats.  DPT 1.008 and 1.007 are what every
// visualisation expects on the first two; using plain DPT 1.001 there would
// still work on the wire but shows up as "Switch" in ETS.
// ─────────────────────────────────────────────────────────────────────────────

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using ShutterMoveCommand =
    CommandPort<LogicalId, bool, Key, DisplayName, application::dptids::UpDown, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using ShutterStopCommand =
    CommandPort<LogicalId, bool, Key, DisplayName, application::dptids::Step, Persisted>;

/// Absolute curtain position, 0 % = fully open.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using ShutterPositionCommand =
    CommandPort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using ShutterPositionState =
    StatePort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

/// Slat/lamella angle as a percentage of the full tilt range.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using SlatAngleCommand =
    CommandPort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using SlatAngleState =
    StatePort<LogicalId, uint8_t, Key, DisplayName, application::dptids::Percent, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Scenes
//
// DPT 18.001 carries both the scene number and a "learn" bit, so one object
// serves recall and store.  DPT 17.001 is the recall-only variant.
// ─────────────────────────────────────────────────────────────────────────────

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using SceneControlCommand = CommandPort<LogicalId,
                                        application::Dpt18Value,
                                        Key,
                                        DisplayName,
                                        application::dptids::SceneControl,
                                        Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using SceneNumberCommand = CommandPort<LogicalId,
                                       application::Dpt17Value,
                                       Key,
                                       DisplayName,
                                       application::dptids::SceneNumber,
                                       Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// HVAC / room control
// ─────────────────────────────────────────────────────────────────────────────

/// Relative setpoint shift in counter pulses (DPT 6.010) — the KNX-standard
/// encoding for a room unit's "warmer/cooler" wheel.  Note this is *not* a
/// temperature in Kelvin: the product decides the step size.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using SetpointShiftCommand = CommandPort<LogicalId,
                                         int8_t,
                                         Key,
                                         DisplayName,
                                         application::dptids::CounterPulsesSigned,
                                         Persisted>;

/// Absolute setpoint shift in Kelvin (DPT 9.002), the alternative encoding.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using SetpointShiftKelvinStateInOut = StateInOutPort<LogicalId,
                                                     float,
                                                     Key,
                                                     DisplayName,
                                                     application::dptids::TemperatureDelta,
                                                     Persisted>;

/// Heating/cooling changeover (DPT 1.100: 0 = cool, 1 = heat).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using HeatCoolStateInOut =
    StateInOutPort<LogicalId, bool, Key, DisplayName, application::dptids::HeatCool, Persisted>;

/// Controller enable/disable (DPT 1.003).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using EnableStateInOut =
    StateInOutPort<LogicalId, bool, Key, DisplayName, application::dptids::Enable, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Binary inputs / status
// ─────────────────────────────────────────────────────────────────────────────

/// Presence/occupancy detector output (DPT 1.018).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using OccupancyState =
    StatePort<LogicalId, bool, Key, DisplayName, application::dptids::Occupancy, Persisted>;

/// Window/door contact (DPT 1.019: 0 = closed, 1 = open).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using WindowDoorState =
    StatePort<LogicalId, bool, Key, DisplayName, application::dptids::WindowDoor, Persisted>;

/// Alarm output (DPT 1.005).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using AlarmState =
    StatePort<LogicalId, bool, Key, DisplayName, application::dptids::Alarm, Persisted>;

/// Alarm acknowledgement input (DPT 1.016).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using AlarmAckCommand =
    CommandPort<LogicalId, bool, Key, DisplayName, application::dptids::AlarmAck, Persisted>;

/// Momentary trigger input (DPT 1.017) — value carries no meaning, only arrival.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using TriggerCommand =
    CommandPort<LogicalId, bool, Key, DisplayName, application::dptids::Trigger, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Metering
// ─────────────────────────────────────────────────────────────────────────────

/// Active electrical energy in Wh (DPT 13.010).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using EnergyState =
    StatePort<LogicalId, int32_t, Key, DisplayName, application::dptids::Energy, Persisted>;

/// Active power in kW (DPT 9.024).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using PowerState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::Power, Persisted>;

/// Generic 16-bit pulse counter (DPT 7.001).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using CounterState =
    StatePort<LogicalId, uint16_t, Key, DisplayName, application::dptids::Counter16, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Environment
// ─────────────────────────────────────────────────────────────────────────────

/// Barometric or differential pressure in Pa (DPT 9.006).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using PressureState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::Pressure, Persisted>;

/// Wind speed in m/s (DPT 9.005).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using WindSpeedState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::WindSpeed, Persisted>;

/// Volume flow in l/h (DPT 9.025).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using VolumeFlowState =
    StatePort<LogicalId, float, Key, DisplayName, application::dptids::VolumeFlow, Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Lighting
// ─────────────────────────────────────────────────────────────────────────────

/// Absolute colour temperature in Kelvin (DPT 7.600), for tunable white.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using ColorTemperatureCommand = CommandPort<LogicalId,
                                            uint16_t,
                                            Key,
                                            DisplayName,
                                            application::dptids::ColorTemperature,
                                            Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using ColorTemperatureState = StatePort<LogicalId,
                                        uint16_t,
                                        Key,
                                        DisplayName,
                                        application::dptids::ColorTemperature,
                                        Persisted>;

/// RGB colour (DPT 232.600).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using RgbColorCommand = CommandPort<LogicalId,
                                    application::Dpt232Value,
                                    Key,
                                    DisplayName,
                                    application::dptids::RgbColor,
                                    Persisted>;

template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using RgbColorState = StatePort<LogicalId,
                                application::Dpt232Value,
                                Key,
                                DisplayName,
                                application::dptids::RgbColor,
                                Persisted>;

// ─────────────────────────────────────────────────────────────────────────────
// Time / date / text
// ─────────────────────────────────────────────────────────────────────────────

/// Time of day (DPT 10.001) — typically received from a bus time master.
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using TimeOfDayCommand = CommandPort<LogicalId,
                                     application::Dpt10Value,
                                     Key,
                                     DisplayName,
                                     application::dptids::TimeOfDay,
                                     Persisted>;

/// Date (DPT 11.001).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = false>
using DateCommand = CommandPort<LogicalId,
                                application::Dpt11Value,
                                Key,
                                DisplayName,
                                application::dptids::Date,
                                Persisted>;

/// 14-character text label (DPT 16.001).
template <auto LogicalId, FixedString Key, FixedString DisplayName = Key, bool Persisted = true>
using TextState =
    StatePort<LogicalId, std::string, Key, DisplayName, application::dptids::Label, Persisted>;

} // namespace knx::product::endpoint::semantics
