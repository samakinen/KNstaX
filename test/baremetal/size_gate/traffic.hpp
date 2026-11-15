// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file traffic.hpp
 * @brief Scripted TP1 medium backend and bus traffic for the emulator variant of the size gate
 *
 * Only the emulator variant (`KNX_SIZE_GATE_SEMIHOST`) includes this file, so it
 * costs the measured image nothing. The backend is a real `Tp1MediumBackend`:
 * the stack hands it frames through `sendFrame()`, and it answers each one with
 * the ACK event a TP1 driver reports (`TxAckResponse`), and it delivers received
 * telegrams through the event callback, as the driver's receive path does. So the
 * stack runs the same code on traffic as on the bus: the receive queue, the data
 * link layer, the transport layer with its connection and acknowledgements, the
 * application layer and the management services.
 *
 * The backend object itself is as small as the null backend (it is a heap object
 * too, and the heap figure includes it); the statistics and the frame log live in
 * static storage of this file.
 *
 * The sequence (`kScript`) is what a device meets in service, in this order:
 * group writes to each datapoint type, a group read answered by the device, a
 * read response, the device's own publishes, and one management session from an
 * individual address: T_Connect, A_PropertyValue_Read and _Write,
 * A_Memory_Write and _Read of the application code and the group address table
 * segments, A_DeviceDescriptor_Read, T_Disconnect.
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace size_gate::traffic {

inline constexpr uint16_t kOwnAddress = 0x110A;   // 1.1.10, set as the device's individual address
inline constexpr uint16_t kPeerAddress = 0x110B;  // 1.1.11, the tool or the other device on the bus

// Group addresses bound to the first port of each datapoint type: 1/0/1 .. 1/0/7.
inline constexpr uint16_t kGroupSwitchIn = 0x0801;
inline constexpr uint16_t kGroupSwitchState = 0x0802;
inline constexpr uint16_t kGroupSwitchIo = 0x0803;
inline constexpr uint16_t kGroupPercent = 0x0804;
inline constexpr uint16_t kGroupDimming = 0x0805;
inline constexpr uint16_t kGroupSceneControl = 0x0806;
inline constexpr uint16_t kGroupSceneNumber = 0x0807;

struct LoggedFrame {
    uint8_t length = 0;
    std::array<uint8_t, 32> bytes{};
};

struct Stats {
    uint32_t injected = 0;       // frames delivered to the stack
    uint32_t sent = 0;           // frames the stack handed to the backend
    uint32_t confirmed = 0;      // ... of which the backend ACKed
    uint32_t pendingAcks = 0;    // sent but not yet confirmed
    uint32_t groupWrites = 0;    // sent: A_GroupValue_Write
    uint32_t groupReads = 0;     // sent: A_GroupValue_Read
    uint32_t groupResponses = 0; // sent: A_GroupValue_Response
    uint32_t linkAcks = 0;       // sent: T_ACK
    uint32_t connectedData = 0;  // sent: T_Data_Connected (management responses)
    uint32_t otherSent = 0;
    uint32_t toolAcksInjected = 0;
    uint32_t pendingToolAcks = 0;  // device data frames that still wait for our T_ACK
    std::array<uint8_t, 4> toolAckSeq{};
    std::array<uint16_t, 16> connectedApci{};  // APCI of the first management responses
    std::array<LoggedFrame, 48> log{};
};

inline Stats g_stats;

inline uint8_t checksum(std::span<const uint8_t> bytes) {
    uint8_t x = 0xFFu;
    for (uint8_t b : bytes) x = static_cast<uint8_t>(x ^ b);
    return x;
}

class ScriptedBackend;
inline ScriptedBackend* g_backend = nullptr;

class ScriptedBackend final : public knx::physical::Tp1MediumBackend {
public:
    ScriptedBackend() = default;
    ~ScriptedBackend() override = default;

    knx::util::Result<void> init(const knx::physical::Tp1MediumConfig&) override {
        state_ = knx::physical::Tp1MediumState::Idle;
        g_backend = this;
        return knx::util::Result<void>::ok();
    }
    void close() override { state_ = knx::physical::Tp1MediumState::Uninitialized; }

    knx::util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        record(frame);
        ++g_stats.pendingAcks;
        return frame.size();
    }

    void setEventCallback(knx::physical::Tp1EventCallback callback, void* context) override {
        callback_ = std::move(callback);
        callbackContext_ = context;
    }

    knx::physical::Tp1MediumState getState() const override { return state_; }

    knx::physical::Tp1CapabilityProfile getCapabilities() const override {
        knx::physical::Tp1CapabilityProfile profile{};
        profile.supportsDetailedTxConfirm = true;  // the driver reports each transmission's ACK
        return profile;
    }

    knx::util::Result<void> setBusMonitorMode(bool) override { return knx::util::Result<void>::ok(); }

    // The driver's service step: report the ACK of the frame sent last.
    knx::util::Result<void> service() override {
        if (g_stats.pendingAcks != 0u && callback_) {
            --g_stats.pendingAcks;
            ++g_stats.confirmed;
            knx::physical::Tp1RxEvent event;
            event.type = knx::physical::Tp1RxEventType::TxAckResponse;
            event.ackClass = knx::physical::Tp1AckClass::Ack;
            callback_(event, callbackContext_);
        }
        return knx::util::Result<void>::ok();
    }

    // A telegram arriving from the bus: body without the checksum.
    void deliver(std::span<const uint8_t> body) {
        std::array<uint8_t, 40> frame{};
        const size_t n = body.size() < 39u ? body.size() : 39u;
        for (size_t i = 0; i < n; ++i) frame[i] = body[i];
        frame[n] = checksum(std::span<const uint8_t>(frame.data(), n));
        knx::physical::Tp1RxEvent event;
        event.type = knx::physical::Tp1RxEventType::TelegramEnd;
        event.frame = std::span<const uint8_t>(frame.data(), n + 1u);
        ++g_stats.injected;
        if (callback_) callback_(event, callbackContext_);
    }

private:
    static void record(std::span<const uint8_t> frame) {
        auto& s = g_stats;
        if (s.sent < s.log.size()) {
            auto& entry = s.log[s.sent];
            entry.length = static_cast<uint8_t>(frame.size() < 32u ? frame.size() : 32u);
            for (size_t i = 0; i < entry.length; ++i) entry.bytes[i] = frame[i];
        }
        ++s.sent;
        if (frame.size() < 8u) {
            ++s.otherSent;
            return;
        }
        const bool group = (frame[5] & 0x80u) != 0u;
        const uint8_t tpci = frame[6];
        if (group) {
            switch (frame[7] & 0xC0u) {
                case 0x00u: ++s.groupReads; break;
                case 0x40u: ++s.groupResponses; break;
                case 0x80u: ++s.groupWrites; break;
                default: ++s.otherSent; break;
            }
        } else if ((tpci & 0xC3u) == 0xC2u || (tpci & 0xC3u) == 0xC3u) {
            ++s.linkAcks;  // T_ACK / T_NAK
        } else if ((tpci & 0xC0u) == 0x40u) {
            // A numbered data frame to the peer: acknowledge it, as the tool does.
            if (s.pendingToolAcks < s.toolAckSeq.size()) {
                s.toolAckSeq[s.pendingToolAcks++] = static_cast<uint8_t>((tpci >> 2) & 0x0Fu);
            }
            const uint16_t apci = static_cast<uint16_t>(((tpci & 0x03u) << 8) | frame[7]);
            if (s.connectedData < s.connectedApci.size()) s.connectedApci[s.connectedData] = apci;
            ++s.connectedData;
        } else {
            ++s.otherSent;
        }
    }

    knx::physical::Tp1MediumState state_{knx::physical::Tp1MediumState::Uninitialized};
    knx::physical::Tp1EventCallback callback_{};
    void* callbackContext_{nullptr};
};

// ── The script ──────────────────────────────────────────────────────────────

struct Step {
    uint16_t atLoop;
    uint8_t length;
    std::array<uint8_t, 16> body;  // ctrl, src(2), dst(2), npci, tpdu...; checksum is added
};

// Individually addressed frames from the peer to the device, system priority.
#define KNX_GATE_TO_DEVICE 0xB0, 0x11, 0x0B, 0x11, 0x0A
// Group frames from the peer, low priority.
#define KNX_GATE_GROUP(hi, lo) 0xBC, 0x11, 0x0B, hi, lo

inline constexpr std::array<Step, 22> kScript{{
    // Group traffic: a write to each datapoint type the device has.
    {20, 8, {KNX_GATE_GROUP(0x08, 0x01), 0xE1, 0x00, 0x81}},         // switch on
    {30, 8, {KNX_GATE_GROUP(0x08, 0x02), 0xE1, 0x00, 0x00}},         // read request: the device answers
    {40, 9, {KNX_GATE_GROUP(0x08, 0x04), 0xE2, 0x00, 0x80, 0x7F}},   // percent
    {50, 8, {KNX_GATE_GROUP(0x08, 0x05), 0xE1, 0x00, 0x89}},         // dimming
    {60, 9, {KNX_GATE_GROUP(0x08, 0x06), 0xE2, 0x00, 0x80, 0x05}},   // scene control
    {70, 9, {KNX_GATE_GROUP(0x08, 0x07), 0xE2, 0x00, 0x80, 0x02}},   // scene number
    {80, 8, {KNX_GATE_GROUP(0x08, 0x03), 0xE1, 0x00, 0x41}},         // read response to the in/out switch
    {90, 8, {KNX_GATE_GROUP(0x08, 0x03), 0xE1, 0x00, 0x00}},         // read request: the device answers
    // (loop 100: the product publishes one value of each type)
    // Management session.
    {150, 7, {KNX_GATE_TO_DEVICE, 0x60, 0x80}},                                        // T_Connect
    {175, 12, {KNX_GATE_TO_DEVICE, 0x65, 0x43, 0xD5, 0x00, 0x0C, 0x10, 0x01}},        // PropertyValue_Read, manufacturer id
    {200, 13, {KNX_GATE_TO_DEVICE, 0x66, 0x47, 0xD7, 0x00, 0x36, 0x10, 0x01, 0x00}},  // PropertyValue_Write, programming mode off
    {225, 14, {KNX_GATE_TO_DEVICE, 0x67, 0x4A, 0x84, 0x01, 0x00, 0x11, 0x22, 0x33, 0x44}},  // Memory_Write 4 B at 0x0100
    {250, 10, {KNX_GATE_TO_DEVICE, 0x63, 0x4E, 0x04, 0x01, 0x00}},                    // Memory_Read 4 B at 0x0100
    {275, 8, {KNX_GATE_TO_DEVICE, 0x61, 0x53, 0x00}},                                  // DeviceDescriptor_Read
    {300, 10, {KNX_GATE_TO_DEVICE, 0x63, 0x56, 0x06, 0x40, 0x00}},                    // Memory_Read 6 B at 0x4000 (group address table)
    {325, 14, {KNX_GATE_TO_DEVICE, 0x67, 0x5A, 0x84, 0x40, 0x00, 0x00, 0x01, 0x08, 0x01}},  // Memory_Write: one group address
    {350, 7, {KNX_GATE_TO_DEVICE, 0x60, 0x81}},                                        // T_Disconnect
    // Group traffic after the session.
    {400, 8, {KNX_GATE_GROUP(0x08, 0x01), 0xE1, 0x00, 0x80}},         // switch off
    {410, 9, {KNX_GATE_GROUP(0x08, 0x04), 0xE2, 0x00, 0x80, 0x20}},   // percent
    {420, 8, {KNX_GATE_GROUP(0x08, 0x02), 0xE1, 0x00, 0x00}},         // read request
    {430, 8, {KNX_GATE_GROUP(0x08, 0x05), 0xE1, 0x00, 0x81}},         // dimming
    {440, 8, {KNX_GATE_GROUP(0x08, 0x03), 0xE1, 0x00, 0x40}},         // read response, off
}};

inline constexpr uint16_t kPublishAtLoop = 100;
inline constexpr uint16_t kRepublishAtLoop = 450;
inline constexpr uint16_t kLoops = 600;

#undef KNX_GATE_TO_DEVICE
#undef KNX_GATE_GROUP


// What the tool does for the numbered data frames the device sent: a T_ACK
// with the same sequence number, one per loop.
inline void deliverToolAck() {
    auto& s = g_stats;
    if (s.pendingToolAcks == 0u || g_backend == nullptr) return;
    const uint8_t seq = s.toolAckSeq[0];
    for (uint32_t i = 1; i < s.pendingToolAcks; ++i) s.toolAckSeq[i - 1] = s.toolAckSeq[i];
    --s.pendingToolAcks;
    const std::array<uint8_t, 7> body{0xB0, 0x11, 0x0B, 0x11, 0x0A, 0x60,
                                      static_cast<uint8_t>(0xC2u | (seq << 2))};
    g_backend->deliver(body);
    ++s.toolAcksInjected;
}

// Delivers whatever the script has for this loop.
inline void tick(uint16_t loop) {
    if (g_backend == nullptr) return;
    for (const Step& step : kScript) {
        if (step.atLoop == loop) {
            g_backend->deliver(std::span<const uint8_t>(step.body.data(), step.length));
        }
    }
    deliverToolAck();
}

}  // namespace size_gate::traffic
