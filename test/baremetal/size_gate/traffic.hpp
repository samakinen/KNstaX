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
 * segments, A_DeviceDescriptor_Read, T_Disconnect. It ends with a full ETS
 * download (see "The ETS download" below): the three tables through the load
 * state machines and the memory segments, group traffic on the downloaded
 * addresses, and A_Restart, which ends the run.
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

inline constexpr size_t kStepBodyBytes = 24;  // the longest frame below: a 12-octet A_Memory_Write is 22

struct Step {
    uint16_t atLoop;
    uint8_t length;
    std::array<uint8_t, kStepBodyBytes> body;  // ctrl, src(2), dst(2), npci, tpdu...; checksum is added
};

// Individually addressed frames from the peer to the device, system priority.
#define KNX_GATE_TO_DEVICE 0xB0, 0x11, 0x0B, 0x11, 0x0A
// Group frames from the peer, low priority.
#define KNX_GATE_GROUP(hi, lo) 0xBC, 0x11, 0x0B, hi, lo

inline constexpr std::array<Step, 22> kBaseScript{{
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

#undef KNX_GATE_TO_DEVICE
#undef KNX_GATE_GROUP

// ── The ETS download ────────────────────────────────────────────────────────
//
// What ETS does to a device with the SystemB (mask 07B0) procedure, as the
// session tests do it (test_ets_connected_session.cpp, downloadLikeEts): for
// the address table, the association table and the group object table in turn,
// PID_LOAD_STATE_CONTROL (5) Unload, StartLoading, the table segment by
// A_Memory_Write (12 octets per frame, the most a 15-octet APDU carries), one
// A_Memory_Read to check it, LoadCompleted; then A_Restart. The reference device
// has no parameters, so there is no application code segment to write. Verify
// Mode is off, as it is for most downloads: the writes are not answered.
//
// The tables are the size of a real I/O module's: 60 group addresses (1/0/1 to
// 1/0/60, one per group object), 80 associations (each object to its own
// address, and 20 objects that also listen to the address of another object of
// the same type) and 60 group object descriptors. Group object n is port n, so
// the address table index of port n is n + 1 and its address is 0x0801 + n.

inline constexpr uint16_t kEtsStartLoop = 600;   // T_Connect
inline constexpr uint16_t kEtsFrameGap = 8;      // loops (5 ms each) per frame: 40 ms, a TP1 frame and its ACK
inline constexpr size_t kEtsAddresses = 60;
inline constexpr size_t kEtsAssociations = 80;
inline constexpr size_t kEtsObjects = 60;
inline constexpr size_t kSegmentChunk = 12;

inline constexpr uint16_t kAddressSegmentBase = 0x4000;      // objects::tableseg
inline constexpr uint16_t kAssociationSegmentBase = 0x4400;
inline constexpr uint16_t kGroupObjectSegmentBase = 0x5000;

/// The second object (0-based port) that listens to the address of port @p port, or -1.
constexpr int extraListener(size_t port) {
    if (port < 8) return static_cast<int>(port + 8);                       // switch commands 1..8 also on 9..16
    if (port >= 16 && port < 20) return static_cast<int>(port + 12);       // switch states 1..4 also on 13..16
    if (port >= 32 && port < 36) return static_cast<int>(port + 4);        // switch in/out 1..4 also on 5..8
    if (port >= 40 && port < 44) return static_cast<int>(port + 4);        // percent 1..4 also on 5..8
    return -1;
}

constexpr uint16_t etsGroupAddress(size_t port) { return static_cast<uint16_t>(0x0801u + port); }

struct Segment {
    std::array<uint8_t, 2 + 4 * kEtsAssociations> bytes{};  // the longest of the three
    size_t length = 0;
};

constexpr void put16(Segment& s, uint16_t v) {
    s.bytes[s.length++] = static_cast<uint8_t>(v >> 8);
    s.bytes[s.length++] = static_cast<uint8_t>(v & 0xFFu);
}

constexpr Segment makeAddressSegment() {
    Segment s;
    put16(s, kEtsAddresses);
    for (size_t port = 0; port < kEtsAddresses; ++port) put16(s, etsGroupAddress(port));
    return s;
}

constexpr Segment makeAssociationSegment() {
    Segment s;
    put16(s, kEtsAssociations);
    uint16_t entries = 0;
    for (size_t port = 0; port < kEtsObjects; ++port) {  // sorted by address table index
        const uint16_t tsap = static_cast<uint16_t>(port + 1u);
        put16(s, tsap);
        put16(s, static_cast<uint16_t>(port));
        ++entries;
        if (const int other = extraListener(port); other >= 0) {
            put16(s, tsap);
            put16(s, static_cast<uint16_t>(other));
            ++entries;
        }
    }
    s.bytes[0] = static_cast<uint8_t>(entries >> 8);
    s.bytes[1] = static_cast<uint8_t>(entries & 0xFFu);
    return s;
}

constexpr Segment makeGroupObjectSegment() {
    Segment s;
    put16(s, kEtsObjects);
    for (size_t port = 0; port < kEtsObjects; ++port) put16(s, 0x0044u);  // descriptor: accepted, not applied
    return s;
}

inline constexpr uint16_t kMaxSteps = 140;

struct Script {
    std::array<Step, kMaxSteps> steps{};
    uint16_t count = 0;
    uint16_t lastLoop = 0;  // the loop of the A_Restart
    uint16_t downloadDoneLoop = 0;  // LoadCompleted of the last table
};

struct ScriptBuilder {
    Script script;
    uint16_t loop = 0;
    uint8_t seq = 0;  // the tool's N(S)

    constexpr Step& next(uint16_t atLoop) {
        Step& step = script.steps[script.count++];
        step.atLoop = atLoop;
        step.length = 0;
        return step;
    }
    static constexpr void put(Step& step, uint8_t v) { step.body[step.length++] = v; }

    /// A numbered management request from the tool: apciHi (the two APCI bits in the TPCI octet),
    /// the APCI octet and the data.
    constexpr void connected(uint8_t apciHi, uint8_t apciLo, const uint8_t* data, size_t count) {
        Step& step = next(loop);
        put(step, 0xB0);
        put(step, 0x11); put(step, 0x0B);  // from 1.1.11
        put(step, 0x11); put(step, 0x0A);  // to 1.1.10
        put(step, static_cast<uint8_t>(0x60u | (1u + count)));
        put(step, static_cast<uint8_t>(0x40u | ((seq & 0x0Fu) << 2) | apciHi));
        put(step, apciLo);
        for (size_t i = 0; i < count; ++i) put(step, data[i]);
        seq = static_cast<uint8_t>((seq + 1u) & 0x0Fu);
        loop = static_cast<uint16_t>(loop + kEtsFrameGap);
    }

    constexpr void unnumbered(uint8_t tpci) {
        Step& step = next(loop);
        for (uint8_t v : {0xB0, 0x11, 0x0B, 0x11, 0x0A, 0x60}) put(step, v);
        put(step, tpci);
        loop = static_cast<uint16_t>(loop + kEtsFrameGap);
    }

    /// PID_LOAD_STATE_CONTROL of interface object @p object: one event octet.
    constexpr void loadEvent(uint8_t object, uint8_t event) {
        const uint8_t data[] = {object, 0x05, 0x10, 0x01, event};
        connected(0x03, 0xD7, data, sizeof(data));  // A_PropertyValue_Write
    }

    constexpr void group(uint16_t address, const uint8_t* apdu, size_t count) {
        Step& step = next(loop);
        for (uint8_t v : {0xBC, 0x11, 0x0B}) put(step, v);
        put(step, static_cast<uint8_t>(address >> 8));
        put(step, static_cast<uint8_t>(address & 0xFFu));
        for (size_t i = 0; i < count; ++i) put(step, apdu[i]);
        loop = static_cast<uint16_t>(loop + kEtsFrameGap);
    }

    constexpr void table(uint8_t object, uint16_t base, const Segment& segment) {
        loadEvent(object, 0x04);  // Unload
        loadEvent(object, 0x01);  // StartLoading
        for (size_t offset = 0; offset < segment.length; offset += kSegmentChunk) {
            const size_t count = segment.length - offset < kSegmentChunk ? segment.length - offset : kSegmentChunk;
            uint8_t data[2 + kSegmentChunk]{};
            const uint16_t address = static_cast<uint16_t>(base + offset);
            data[0] = static_cast<uint8_t>(address >> 8);
            data[1] = static_cast<uint8_t>(address & 0xFFu);
            for (size_t i = 0; i < count; ++i) data[2 + i] = segment.bytes[offset + i];
            connected(0x02, static_cast<uint8_t>(0x80u | count), data, 2 + count);  // A_Memory_Write
        }
        const uint8_t verify[] = {static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base & 0xFFu)};
        connected(0x02, static_cast<uint8_t>(0x00u | kSegmentChunk), verify, 2);  // A_Memory_Read of the first chunk
        loadEvent(object, 0x02);  // LoadCompleted
    }
};

constexpr Script makeScript() {
    ScriptBuilder b;
    for (const Step& step : kBaseScript) b.script.steps[b.script.count++] = step;

    b.loop = kEtsStartLoop;
    b.unnumbered(0x80);  // T_Connect
    b.table(1, kAddressSegmentBase, makeAddressSegment());
    b.table(2, kAssociationSegmentBase, makeAssociationSegment());
    b.table(3, kGroupObjectSegmentBase, makeGroupObjectSegment());
    b.script.downloadDoneLoop = static_cast<uint16_t>(b.loop - kEtsFrameGap);

    // Group traffic on the downloaded addresses, while the tables are still unsaved. Each
    // frame names the port it reaches: port n is on 0x0801 + n.
    const uint8_t sw[] = {0xE1, 0x00, 0x81};                // port 0 and 8 (command): switch on
    const uint8_t percent[] = {0xE2, 0x00, 0x80, 0x7F};     // port 40 and 44
    const uint8_t read[] = {0xE1, 0x00, 0x00};              // read request
    const uint8_t dimming[] = {0xE1, 0x00, 0x89};           // port 48
    const uint8_t sceneControl[] = {0xE2, 0x00, 0x80, 0x05};  // port 52
    const uint8_t sceneNumber[] = {0xE2, 0x00, 0x80, 0x02};   // port 56
    const uint8_t response[] = {0xE1, 0x00, 0x41};          // read response, to port 32 and 36
    b.group(etsGroupAddress(0), sw, sizeof(sw));
    b.group(etsGroupAddress(40), percent, sizeof(percent));
    b.group(etsGroupAddress(16), read, sizeof(read));       // switch state 1 answers
    b.group(etsGroupAddress(48), dimming, sizeof(dimming));
    b.group(etsGroupAddress(52), sceneControl, sizeof(sceneControl));
    b.group(etsGroupAddress(56), sceneNumber, sizeof(sceneNumber));
    b.group(etsGroupAddress(32), response, sizeof(response));
    b.group(etsGroupAddress(32), read, sizeof(read));       // switch in/out 1 answers

    b.connected(0x03, 0x80, nullptr, 0);  // A_Restart, basic restart
    b.script.lastLoop = static_cast<uint16_t>(b.loop - kEtsFrameGap);
    return b.script;
}

inline constexpr Script kScriptData = makeScript();
static_assert(kScriptData.count <= kMaxSteps, "raise kMaxSteps");

/// Values the inbound ports observe in all: 8 before the download, and after it one write on
/// ports 0 and 8, one on 40 and 44, and one each on 48, 52 and 56.
inline constexpr uint32_t kExpectedObserved = 8u + 7u;

inline constexpr uint16_t kLoops = kScriptData.lastLoop + 200u;  // the run ends at the restart; this is the cap


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
    for (uint16_t i = 0; i < kScriptData.count; ++i) {
        const Step& step = kScriptData.steps[i];
        if (step.atLoop == loop) {
            g_backend->deliver(std::span<const uint8_t>(step.body.data(), step.length));
        }
    }
    deliverToolAck();
}

}  // namespace size_gate::traffic
