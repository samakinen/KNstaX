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
    
    uint64_t seq = sec.incrementSendingSequence();
    TEST_ASSERT_TRUE(seq == 1);
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

    // Populate loadable content plus a tool key.
    std::array<uint8_t, 16> key;
    key.fill(0xAB);
    sec.setToolKey(key);
    sec.setGroupKey(GroupAddress(1, 2, 3), key);
    sec.setDeviceKey(IndividualAddress(1, 1, 5), key);
    TEST_ASSERT_EQUAL_UINT16(1, sec.groupKeyCount());
    TEST_ASSERT_EQUAL_UINT16(1, sec.deviceKeyCount());

    // StartLoading discards downloaded key tables but must keep the tool key,
    // which is the credential ETS is using for this very download.
    const uint8_t startLoading[] = {loadstate::kEventStartLoading};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, startLoading).isOk());
    TEST_ASSERT_EQUAL_UINT16(0, sec.groupKeyCount());
    TEST_ASSERT_EQUAL_UINT16(0, sec.deviceKeyCount());
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

    // Unload clears the tables again and returns to Unloaded.
    sec.setGroupKey(GroupAddress(1, 2, 3), key);
    const uint8_t unload[] = {loadstate::kEventUnload};
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, pid, unload).isOk());
    TEST_ASSERT_EQUAL_UINT16(0, sec.groupKeyCount());
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
    
    return UNITY_END();
}
