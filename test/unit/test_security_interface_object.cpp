// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_security_interface_object.cpp
 * @brief Security Interface Object tests - simplified for Unity
 */

#include "unity.h"
#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/property_kernel.hpp"

#include <span>
#include <vector>

using namespace knx;
using namespace knx::objects;

static util::Result<void> readSecurityProperty(const SecurityInterfaceObject& obj,
                                               application::PropertyID id,
                                               uint16_t elementCount,
                                               std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

void setUp(void) {}
void tearDown(void) {}

void test_SecurityInterfaceObject_DefaultConstruction() {
    SecurityInterfaceObject sec;
    
    TEST_ASSERT_EQUAL(SecurityMode::Disabled, sec.getSecurityMode());
    TEST_ASSERT_FALSE(sec.isSecurityEnabled());
    TEST_ASSERT_FALSE(sec.hasToolKey());
    TEST_ASSERT_EQUAL_UINT16(0, sec.deviceKeyCount());
    TEST_ASSERT_EQUAL_UINT16(0, sec.groupKeyCount());
}

void test_SecurityInterfaceObject_SecurityMode() {
    SecurityInterfaceObject sec;
    
    sec.setSecurityMode(SecurityMode::Enabled);
    TEST_ASSERT_TRUE(sec.isSecurityEnabled());
    
    sec.setSecurityMode(SecurityMode::Disabled);
    TEST_ASSERT_FALSE(sec.isSecurityEnabled());
}

void test_SecurityInterfaceObject_ToolKey() {
    SecurityInterfaceObject sec;
    
    std::array<uint8_t, 16> key;
    key[0] = 1; // Non-zero key
    sec.setToolKey(key);
    TEST_ASSERT_TRUE(sec.hasToolKey());
}

void test_SecurityInterfaceObject_DeviceKeys() {
    SecurityInterfaceObject sec;
    
    IndividualAddress addr(1, 2, 3);
    std::array<uint8_t, 16> key;
    key.fill(42);
    
    sec.setDeviceKey(addr, key);
    TEST_ASSERT_EQUAL_UINT16(1, sec.deviceKeyCount());
    
    std::array<uint8_t, 16> retrieved;
    TEST_ASSERT_TRUE(sec.getDeviceKey(addr, retrieved).isOk());
    
    sec.removeDeviceKey(addr);
    TEST_ASSERT_EQUAL_UINT16(0, sec.deviceKeyCount());
}

void test_SecurityInterfaceObject_GroupKeys() {
    SecurityInterfaceObject sec;
    
    GroupAddress addr(1, 2, 3);
    std::array<uint8_t, 16> key;
    key.fill(99);
    
    sec.setGroupKey(addr, key);
    TEST_ASSERT_EQUAL_UINT16(1, sec.groupKeyCount());
    
    std::array<uint8_t, 16> retrieved;
    TEST_ASSERT_TRUE(sec.getGroupKey(addr, retrieved).isOk());
    
    sec.removeGroupKey(addr);
    TEST_ASSERT_EQUAL_UINT16(0, sec.groupKeyCount());
}

void test_SecurityInterfaceObject_Sequences() {
    SecurityInterfaceObject sec;
    
    // No reserve writer (no persistence): the counter is unlimited.
    const auto seq = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(seq.isOk());
    TEST_ASSERT_TRUE(seq.value() == 1);
    TEST_ASSERT_TRUE(sec.getSendingSequence() == 1);
    
    auto ok = sec.updateReceivingSequence(100);
    TEST_ASSERT_TRUE(ok.isOk());
    TEST_ASSERT_TRUE(sec.getReceivingSequence() == 100);
    
    // Invalid update
    ok = sec.updateReceivingSequence(50);
    TEST_ASSERT_FALSE(ok.isOk());
}

void test_SecurityInterfaceObject_Failures() {
    SecurityInterfaceObject sec;
    
    sec.logSecurityFailure(SecurityFailure::AuthenticationFailed, 0x1234);
    TEST_ASSERT_EQUAL(SecurityFailure::AuthenticationFailed, sec.getLastFailure());
    TEST_ASSERT_EQUAL_UINT32(1, sec.getFailureCount());
    
    sec.clearFailureLog();
    TEST_ASSERT_EQUAL_UINT32(0, sec.getFailureCount());
}

void test_SecurityInterfaceObject_Properties() {
    SecurityInterfaceObject sec;
    std::vector<uint8_t> value;
    
    auto ok = readSecurityProperty(sec,
                                   static_cast<application::PropertyID>(SecurityProperty::ObjectType),
                                   1,
                                   value);
    TEST_ASSERT_TRUE(ok.isOk());
    TEST_ASSERT_EQUAL_UINT16(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x11, value[1]);
}

void test_SecurityInterfaceObject_ClearKeys() {
    SecurityInterfaceObject sec;
    
    std::array<uint8_t, 16> key;
    key.fill(1);
    sec.setToolKey(key);
    
    sec.clearAllKeys();
    TEST_ASSERT_FALSE(sec.hasToolKey());
}

void test_SecurityInterfaceObject_Validation() {
    SecurityInterfaceObject sec;
    
    TEST_ASSERT_TRUE(sec.isValid()); // Disabled is valid
    
    sec.setSecurityMode(SecurityMode::Enabled);
    TEST_ASSERT_FALSE(sec.isValid()); // Enabled without key is invalid
    
    std::array<uint8_t, 16> key;
    key.fill(1);
    sec.setToolKey(key);
    TEST_ASSERT_TRUE(sec.isValid()); // Enabled with key is valid
}

static util::Result<void> writeSecurityProperty(SecurityInterfaceObject& obj,
                                                application::PropertyID id,
                                                std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

// Regression: without PID_LOAD_STATE_CONTROL (5) the Data Secure key-table
// download aborts at StartLoading, because ETS runs the standard load
// procedure against the Security Interface Object like any other loadable one.
void test_SecurityInterfaceObject_LoadStateControl() {
    SecurityInterfaceObject sec;
    const auto pid = static_cast<application::PropertyID>(SecurityProperty::LoadStateControl);

    // Key tables are not firmware-defined, so the object boots Unloaded.
    std::vector<uint8_t> out;
    TEST_ASSERT_TRUE(readSecurityProperty(sec, pid, 1, out).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, out.size());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kUnloaded, out[0]);

    // Populate downloaded content plus a tool key.
    std::array<uint8_t, 16> key;
    key.fill(0xAB);
    sec.setToolKey(key);
    sec.applyDownloadedGroupKeys({{GroupAddress(1, 2, 3).raw, key}}, true);
    sec.applyDownloadedDeviceKeys({{IndividualAddress(1, 1, 5).raw, key}}, true);
    TEST_ASSERT_EQUAL_UINT16(1, sec.groupKeyCount());
    TEST_ASSERT_EQUAL_UINT16(1, sec.deviceKeyCount());

    // StartLoading discards downloaded key tables but must keep the tool key,
    // which is the credential ETS is using for this very download.
    const uint8_t startLoading[] = {loadstate::kEventStartLoading};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, startLoading).isOk());
    TEST_ASSERT_EQUAL_UINT16(0, sec.groupKeyCount());
    TEST_ASSERT_EQUAL_UINT16(0, sec.deviceKeyCount());
    TEST_ASSERT_FALSE(sec.downloadedGroupKeysInForce());
    TEST_ASSERT_TRUE(sec.hasToolKey());

    out.clear();
    TEST_ASSERT_TRUE(readSecurityProperty(sec, pid, 1, out).isOk());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoading, out[0]);

    // ETS appends additional load-control data to the event octet; the block
    // must be accepted rather than rejected as a malformed write.
    const uint8_t additional[] = {loadstate::kEventAdditionalLoadControls, 0x00, 0x01, 0x02, 0x03};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, additional).isOk());
    out.clear();
    TEST_ASSERT_TRUE(readSecurityProperty(sec, pid, 1, out).isOk());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoading, out[0]);

    const uint8_t loadCompleted[] = {loadstate::kEventLoadCompleted};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, loadCompleted).isOk());
    out.clear();
    TEST_ASSERT_TRUE(readSecurityProperty(sec, pid, 1, out).isOk());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoaded, out[0]);

    // Unload clears the tables again and returns to Unloaded. A firmware key
    // is the factory default and is what is in force once the table is gone.
    sec.setGroupKey(GroupAddress(1, 2, 4), key);
    sec.applyDownloadedGroupKeys({{GroupAddress(1, 2, 3).raw, key}}, true);
    std::array<uint8_t, 16> found{};
    TEST_ASSERT_FALSE(sec.getGroupKey(GroupAddress(1, 2, 4), found).isOk());
    const uint8_t unload[] = {loadstate::kEventUnload};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, unload).isOk());
    TEST_ASSERT_EQUAL_UINT16(1, sec.groupKeyCount());
    TEST_ASSERT_FALSE(sec.getGroupKey(GroupAddress(1, 2, 3), found).isOk());
    TEST_ASSERT_TRUE(sec.getGroupKey(GroupAddress(1, 2, 4), found).isOk());
    out.clear();
    TEST_ASSERT_TRUE(readSecurityProperty(sec, pid, 1, out).isOk());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kUnloaded, out[0]);
}

namespace {

// One PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE element: IA(2) + Last Valid SeqNr(6).
void appendIaEntry(std::vector<uint8_t>& table, const IndividualAddress& address, uint64_t sequence) {
    table.push_back(static_cast<uint8_t>(address.raw >> 8));
    table.push_back(static_cast<uint8_t>(address.raw & 0xFFu));
    for (int shift = 40; shift >= 0; shift -= 8) {
        table.push_back(static_cast<uint8_t>((sequence >> shift) & 0xFFu));
    }
}

} // namespace

// 03/05/01 §6.3.8.4: the Last Valid SeqNr of every partner is part of the
// Security Individual Address Table, and the MaC "shall write this value in the
// field Last Valid SeqNr" when it knows it. Ignoring the field — as the earlier
// parser did, reading only the IA — starts every partner's replay window at 0.
void test_SecurityInterfaceObject_AdoptsLastValidSeqNrFromAddressTable() {
    SecurityInterfaceObject sec;
    const IndividualAddress tool(1, 1, 1);
    const IndividualAddress peer(1, 1, 7);

    std::vector<uint8_t> table;
    appendIaEntry(table, tool, 0x0000000012345678ull);
    appendIaEntry(table, peer, 42u);
    TEST_ASSERT_TRUE(writeSecurityProperty(
        sec,
        static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable),
        table).isOk());

    sec.syncSequencesFromAddressTable();

    TEST_ASSERT_TRUE(sec.getPeerSequence(tool) == 0x0000000012345678ull);
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 42u);
    // A partner with no entry has no secure link, and no accepted number.
    TEST_ASSERT_TRUE(sec.getPeerSequence(IndividualAddress(1, 1, 9)) == 0u);
}

// The same table is where an accepted sequence number has to end up, because
// the table is a persisted property and the live map is not. §6.3.8.4: "it
// shall update the field Last Valid SeqNr for that IA in the Security
// Individual Address Table with the received value".
void test_SecurityInterfaceObject_AcceptedSeqNrIsWrittenIntoAddressTable() {
    SecurityInterfaceObject sec;
    const IndividualAddress peer(1, 1, 7);

    std::vector<uint8_t> table;
    appendIaEntry(table, IndividualAddress(1, 1, 1), 0u);
    appendIaEntry(table, peer, 10u);
    TEST_ASSERT_TRUE(writeSecurityProperty(
        sec,
        static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable),
        table).isOk());
    sec.syncSequencesFromAddressTable();
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());

    sec.setPeerSequence(peer, 0x0000AABBCCDDEEllu);

    const auto* stored = sec.findExtraProperty(SecurityProperty::SecurityIndividualAddressTable);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_UINT32(16u, stored->size());
    // Second element, sequence field: 0x00AABBCCDDEE.
    const uint8_t expected[] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], (*stored)[8 + 2 + i]);
    }
    // The first entry is untouched, and a checkpoint is now owed.
    TEST_ASSERT_EQUAL_HEX8(0x00, (*stored)[2]);
    TEST_ASSERT_TRUE(sec.sequenceStateDirty());

    sec.clearSequenceStateDirty();
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());
    // Re-accepting the same number is not a state change worth a flash write.
    sec.setPeerSequence(peer, 0x0000AABBCCDDEEllu);
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());
}

// Reconciliation keeps the higher of the two sides: a table ETS downloaded
// while this device was already running must not roll a replay window
// backwards, and neither must a restored table that is behind the live state.
void test_SecurityInterfaceObject_SequenceSyncNeverMovesBackwards() {
    SecurityInterfaceObject sec;
    const IndividualAddress peer(1, 1, 7);

    std::vector<uint8_t> table;
    appendIaEntry(table, peer, 100u);
    TEST_ASSERT_TRUE(writeSecurityProperty(
        sec,
        static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable),
        table).isOk());

    sec.setPeerSequence(peer, 500u);
    sec.syncSequencesFromAddressTable();
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 500u);

    // …and the table is brought up to the live value, so the next checkpoint
    // stores 500 rather than resurrecting 100.
    const auto* stored = sec.findExtraProperty(SecurityProperty::SecurityIndividualAddressTable);
    TEST_ASSERT_NOT_NULL(stored);
    TEST_ASSERT_EQUAL_HEX8(0x01, (*stored)[6]);
    TEST_ASSERT_EQUAL_HEX8(0xF4, (*stored)[7]);
}

// --- Sending sequence reserve (docs/design/data-secure-sequence-persistence.md)

namespace {

struct CeilingStore {
    std::vector<uint64_t> writes;
    bool fail{false};

    static bool write(void* context, uint64_t ceiling)
    {
        auto* self = static_cast<CeilingStore*>(context);
        if (self->fail) {
            return false;
        }
        self->writes.push_back(ceiling);
        return true;
    }
};

}  // namespace

// The first send stores a ceiling a window ahead; the numbers below it need no
// further write, and a new ceiling is due once half of the window is used.
void test_SecurityInterfaceObject_SendingReserveWritesOncePerWindow() {
    SecurityInterfaceObject sec;
    CeilingStore store;
    sec.setSendingReserveWriter(&CeilingStore::write, &store, 16u);

    auto first = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(first.isOk());
    TEST_ASSERT_TRUE(first.value() == 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, store.writes.size());
    TEST_ASSERT_TRUE(store.writes[0] == 16u);
    TEST_ASSERT_TRUE(sec.sendingReserveCeiling() == 16u);

    // Up to 8 the remaining window is at least half: no top-up owed.
    for (uint64_t expected = 2u; expected <= 8u; ++expected) {
        auto next = sec.incrementSendingSequence();
        TEST_ASSERT_TRUE(next.isOk());
        TEST_ASSERT_TRUE(next.value() == expected);
    }
    TEST_ASSERT_FALSE(sec.sendingReserveLow());
    TEST_ASSERT_EQUAL_UINT32(1u, store.writes.size());

    // 9 leaves 7 of 16: the background top-up is now owed, but the send itself
    // does not write.
    TEST_ASSERT_TRUE(sec.incrementSendingSequence().isOk());
    TEST_ASSERT_TRUE(sec.sendingReserveLow());
    TEST_ASSERT_EQUAL_UINT32(1u, store.writes.size());

    TEST_ASSERT_TRUE(sec.topUpSendingReserve());
    TEST_ASSERT_FALSE(sec.sendingReserveLow());
    TEST_ASSERT_EQUAL_UINT32(2u, store.writes.size());
    TEST_ASSERT_TRUE(store.writes[1] == 9u + 16u);

    // A top-up with nothing to gain writes nothing.
    TEST_ASSERT_TRUE(sec.topUpSendingReserve());
    TEST_ASSERT_EQUAL_UINT32(2u, store.writes.size());
}

// Without a top-up the window runs out: the send that needs a number above the
// ceiling stores a new one itself.
void test_SecurityInterfaceObject_SendingReserveExhaustedWritesBeforeSending() {
    SecurityInterfaceObject sec;
    CeilingStore store;
    sec.setSendingReserveWriter(&CeilingStore::write, &store, 4u);

    for (uint64_t expected = 1u; expected <= 4u; ++expected) {
        TEST_ASSERT_TRUE(sec.incrementSendingSequence().value() == expected);
    }
    TEST_ASSERT_EQUAL_UINT32(1u, store.writes.size());

    TEST_ASSERT_TRUE(sec.incrementSendingSequence().value() == 5u);
    TEST_ASSERT_EQUAL_UINT32(2u, store.writes.size());
    TEST_ASSERT_TRUE(store.writes[1] == 4u + 4u);
}

// A number that could not be reserved is not taken, and the counter does not
// move: the caller refuses the send and a later attempt gets the same number.
void test_SecurityInterfaceObject_SendingReserveFailureRefusesTheNumber() {
    SecurityInterfaceObject sec;
    CeilingStore store;
    store.fail = true;
    sec.setSendingReserveWriter(&CeilingStore::write, &store, 64u);

    const auto refused = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(refused.isError());
    TEST_ASSERT_TRUE(refused.error() == util::ErrorCode::ResourceUnavailable);
    TEST_ASSERT_TRUE(sec.getSendingSequence() == 0u);
    TEST_ASSERT_FALSE(sec.topUpSendingReserve());

    store.fail = false;
    const auto taken = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(taken.isOk());
    TEST_ASSERT_TRUE(taken.value() == 1u);
}

// After a reset every number up to the stored ceiling may have been used: the
// counter moves up to it, and the first send reserves a new window. Restoring
// writes nothing, so a device that never sends secured never writes.
void test_SecurityInterfaceObject_RestoredCeilingRaisesTheCounter() {
    SecurityInterfaceObject sec;
    CeilingStore store;
    sec.setSendingSequenceSilent(100u);  // the property from the last full save
    sec.setSendingReserveWriter(&CeilingStore::write, &store, 1000u);

    sec.restoreSendingReserveCeiling(5000u);
    TEST_ASSERT_TRUE(sec.getSendingSequence() == 5000u);
    TEST_ASSERT_FALSE(sec.sendingReserveLow());
    TEST_ASSERT_EQUAL_UINT32(0u, store.writes.size());

    const auto next = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(next.value() == 5001u);
    TEST_ASSERT_EQUAL_UINT32(1u, store.writes.size());
    TEST_ASSERT_TRUE(store.writes[0] == 6000u);

    // A ceiling behind the counter (a newer full save) never moves it back.
    SecurityInterfaceObject ahead;
    ahead.setSendingSequenceSilent(7000u);
    ahead.restoreSendingReserveCeiling(5000u);
    TEST_ASSERT_TRUE(ahead.getSendingSequence() == 7000u);
}

// The two checkpoint records have their own flags: Tool Access moves only while
// ETS talks to the device, so the partner table alone is written otherwise.
void test_SecurityInterfaceObject_CheckpointRecordsHaveSeparateFlags() {
    SecurityInterfaceObject sec;
    sec.setPeerSequence(IndividualAddress(1, 1, 7), 10u);
    TEST_ASSERT_TRUE(sec.peerSequencesDirty());
    TEST_ASSERT_FALSE(sec.toolAccessSequenceDirty());

    TEST_ASSERT_TRUE(sec.takePeerSequencesDirty());
    TEST_ASSERT_FALSE(sec.peerSequencesDirty());
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());

    sec.setToolAccessSequence(3u);
    TEST_ASSERT_TRUE(sec.toolAccessSequenceDirty());
    TEST_ASSERT_FALSE(sec.peerSequencesDirty());
    TEST_ASSERT_TRUE(sec.sequenceStateDirty());

    // A failed write marks the record back.
    TEST_ASSERT_TRUE(sec.takeToolAccessSequenceDirty());
    sec.markToolAccessSequenceDirty();
    TEST_ASSERT_TRUE(sec.toolAccessSequenceDirty());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_SecurityInterfaceObject_DefaultConstruction);
    RUN_TEST(test_SecurityInterfaceObject_SecurityMode);
    RUN_TEST(test_SecurityInterfaceObject_ToolKey);
    RUN_TEST(test_SecurityInterfaceObject_DeviceKeys);
    RUN_TEST(test_SecurityInterfaceObject_GroupKeys);
    RUN_TEST(test_SecurityInterfaceObject_Sequences);
    RUN_TEST(test_SecurityInterfaceObject_Failures);
    RUN_TEST(test_SecurityInterfaceObject_Properties);
    RUN_TEST(test_SecurityInterfaceObject_ClearKeys);
    RUN_TEST(test_SecurityInterfaceObject_Validation);
    RUN_TEST(test_SecurityInterfaceObject_LoadStateControl);
    RUN_TEST(test_SecurityInterfaceObject_AdoptsLastValidSeqNrFromAddressTable);
    RUN_TEST(test_SecurityInterfaceObject_AcceptedSeqNrIsWrittenIntoAddressTable);
    RUN_TEST(test_SecurityInterfaceObject_SequenceSyncNeverMovesBackwards);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveWritesOncePerWindow);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveExhaustedWritesBeforeSending);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveFailureRefusesTheNumber);
    RUN_TEST(test_SecurityInterfaceObject_RestoredCeilingRaisesTheCounter);
    RUN_TEST(test_SecurityInterfaceObject_CheckpointRecordsHaveSeparateFlags);

    return UNITY_END();
}
