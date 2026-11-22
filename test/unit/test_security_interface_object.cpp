// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_security_interface_object.cpp
 * @brief Security Interface Object tests - simplified for Unity
 */

#include "unity.h"
#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/property_kernel.hpp"

#include <array>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::objects;

// Allocation counter. A device locks its heap after start-up
// (KNX_HEAP_POLICY_INIT_ONLY), and on the host the policy does not replace
// operator new, so this program does: every allocation made while counting is
// one the device would refuse.
namespace {
bool g_countAllocations = false;
size_t g_allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    if (g_countAllocations) {
        ++g_allocations;
    }
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static util::Result<void> readSecurityProperty(const SecurityInterfaceObject& obj,
                                               application::PropertyID id,
                                               uint16_t elementCount,
                                               std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeSecurityProperty(SecurityInterfaceObject& obj,
                                                application::PropertyID id,
                                                std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

namespace {

constexpr auto kPidP2PKeyTable = static_cast<application::PropertyID>(SecurityProperty::P2PKeyTable);
constexpr auto kPidGroupKeyTable = static_cast<application::PropertyID>(SecurityProperty::GroupKeyTable);
constexpr auto kPidIaTable =
    static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable);

// One PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE element: IA(2) + Last Valid SeqNr(6).
void appendIaEntry(std::vector<uint8_t>& table, const IndividualAddress& address, uint64_t sequence) {
    table.push_back(static_cast<uint8_t>(address.raw >> 8));
    table.push_back(static_cast<uint8_t>(address.raw & 0xFFu));
    for (int shift = 40; shift >= 0; shift -= 8) {
        table.push_back(static_cast<uint8_t>((sequence >> shift) & 0xFFu));
    }
}

// One key table element: index(2) + key(16), and Roles(2) for the P2P table.
void appendKeyEntry(std::vector<uint8_t>& table, uint16_t index, const std::array<uint8_t, 16>& key,
                    bool withRoles) {
    table.push_back(static_cast<uint8_t>(index >> 8));
    table.push_back(static_cast<uint8_t>(index & 0xFFu));
    table.insert(table.end(), key.begin(), key.end());
    if (withRoles) {
        table.push_back(0);
        table.push_back(0);
    }
}

// A group address table of three entries: GA_Index 1, 2, 3.
const GroupAddress kAddressTable[] = {GroupAddress(1, 2, 3), GroupAddress(1, 2, 4), GroupAddress(1, 2, 5)};

GroupAddress addressAt(const void* /*context*/, uint16_t gaIndex) {
    if (gaIndex == 0 || gaIndex > std::size(kAddressTable)) {
        return GroupAddress(GroupAddress::invalidValue());
    }
    return kAddressTable[gaIndex - 1u];
}

}  // namespace

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
    sec.setGroupAddressResolver(&addressAt, nullptr);
    std::vector<uint8_t> groupKeys;
    appendKeyEntry(groupKeys, 1, key, false);  // 1/2/3
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidGroupKeyTable, groupKeys).isOk());
    std::vector<uint8_t> addresses;
    appendIaEntry(addresses, IndividualAddress(1, 1, 5), 0u);
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, addresses).isOk());
    std::vector<uint8_t> deviceKeys;
    appendKeyEntry(deviceKeys, 1, key, true);  // 1.1.5
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidP2PKeyTable, deviceKeys).isOk());
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
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidGroupKeyTable, groupKeys).isOk());
    std::array<uint8_t, 16> found{};
    TEST_ASSERT_TRUE(sec.getGroupKey(GroupAddress(1, 2, 3), found).isOk());
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

    TEST_ASSERT_TRUE(sec.getPeerSequence(tool) == 0x0000000012345678ull);
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 42u);
    // A partner with no entry has no secure link, and no accepted number.
    TEST_ASSERT_TRUE(sec.getPeerSequence(IndividualAddress(1, 1, 9)) == 0u);
}

// The same table is where an accepted sequence number has to end up, because
// the table is a persisted property. §6.3.8.4: "it
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
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());

    TEST_ASSERT_TRUE(sec.setPeerSequence(peer, 0x0000AABBCCDDEEllu).isOk());

    const auto stored = sec.tableData(SecurityProperty::SecurityIndividualAddressTable);
    TEST_ASSERT_EQUAL_UINT32(16u, stored.size());
    // Second element, sequence field: 0x00AABBCCDDEE.
    const uint8_t expected[] = {0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], stored[8 + 2 + i]);
    }
    // The first entry is untouched, and a checkpoint is now owed.
    TEST_ASSERT_EQUAL_HEX8(0x00, stored[2]);
    TEST_ASSERT_TRUE(sec.sequenceStateDirty());

    sec.clearSequenceStateDirty();
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());
    // Re-accepting the same number is not a state change worth a flash write.
    TEST_ASSERT_TRUE(sec.setPeerSequence(peer, 0x0000AABBCCDDEEllu).isOk());
    TEST_ASSERT_FALSE(sec.sequenceStateDirty());
}

// A download never moves a partner's number back: ETS empties the table and
// writes it again with its own idea of the numbers, and each partner keeps
// the higher of the two. The same holds for an entry written in place.
void test_SecurityInterfaceObject_DownloadNeverMovesSequencesBackwards() {
    SecurityInterfaceObject sec;
    const IndividualAddress peer(1, 1, 7);
    const IndividualAddress other(1, 1, 8);

    std::vector<uint8_t> table;
    appendIaEntry(table, peer, 100u);
    appendIaEntry(table, other, 0u);
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, table).isOk());
    TEST_ASSERT_TRUE(sec.setPeerSequence(peer, 500u).isOk());

    // StartLoading empties every table; the number waits in an unlisted slot.
    sec.loadControlReset();
    TEST_ASSERT_EQUAL_UINT32(0u, sec.tableData(SecurityProperty::SecurityIndividualAddressTable).size());
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 500u);

    // ETS shortens the table to one element, then writes both entries again
    // with the numbers from its project.
    const auto binding = sec.kernelBinding();
    TEST_ASSERT_TRUE(resizeProperty(binding.handlers, binding.handlerCount, binding.context,
                                    kPidIaTable, 1).isOk());
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, table).isOk());
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 500u);
    TEST_ASSERT_TRUE(sec.getPeerSequence(other) == 0u);

    // The table itself holds 500, so the next checkpoint stores it.
    const auto stored = sec.tableData(SecurityProperty::SecurityIndividualAddressTable);
    TEST_ASSERT_EQUAL_HEX8(0x01, stored[6]);
    TEST_ASSERT_EQUAL_HEX8(0xF4, stored[7]);

    // An entry written in place with a lower number keeps the higher one.
    std::vector<uint8_t> stale;
    appendIaEntry(stale, peer, 7u);
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, stale).isOk());
    TEST_ASSERT_TRUE(sec.getPeerSequence(peer) == 500u);
}

// The tables are fixed arrays of the configured capacity, reported to ETS as
// the property's maximum: a write past it is refused, and the table keeps
// what it had.
void test_SecurityInterfaceObject_TablesRefuseWritesPastTheirCapacity() {
    SecurityInterfaceObject sec;
    const uint16_t capacity = sec.tableCapacity(SecurityProperty::P2PKeyTable);
    TEST_ASSERT_EQUAL_UINT16(knx::config::SECURITY_P2P_KEYS, capacity);

    std::array<uint8_t, 16> key{};
    key.fill(0x5A);
    std::vector<uint8_t> full;
    for (uint16_t i = 1; i <= capacity; ++i) {
        appendKeyEntry(full, i, key, true);
    }
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidP2PKeyTable, full).isOk());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(capacity) * 20u,
                             sec.tableData(SecurityProperty::P2PKeyTable).size());

    std::vector<uint8_t> oneMore;
    appendKeyEntry(oneMore, static_cast<uint16_t>(capacity + 1u), key, true);
    const auto binding = sec.kernelBinding();
    TEST_ASSERT_TRUE(writeProperty(binding.handlers, binding.handlerCount, binding.context,
                                   kPidP2PKeyTable, static_cast<uint16_t>(capacity + 1u), oneMore)
                         .isError());
    TEST_ASSERT_TRUE(resizeProperty(binding.handlers, binding.handlerCount, binding.context,
                                    kPidP2PKeyTable, static_cast<uint16_t>(capacity + 1u))
                         .isError());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(capacity) * 20u,
                             sec.tableData(SecurityProperty::P2PKeyTable).size());
}

// A downloaded group key is found through the address table: the entry names
// GA_Index 2, which is 1/2/4 there. Without the address table, or with an index
// it does not have, the key resolves to nothing.
void test_SecurityInterfaceObject_GroupKeyResolvesThroughTheAddressTable() {
    SecurityInterfaceObject sec;
    std::array<uint8_t, 16> key{};
    key.fill(0x77);
    std::vector<uint8_t> groupKeys;
    appendKeyEntry(groupKeys, 2, key, false);
    appendKeyEntry(groupKeys, 9, key, false);  // no such GA_Index
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidGroupKeyTable, groupKeys).isOk());

    std::array<uint8_t, 16> found{};
    TEST_ASSERT_FALSE(sec.getGroupKey(GroupAddress(1, 2, 4), found).isOk());

    sec.setGroupAddressResolver(&addressAt, nullptr);
    TEST_ASSERT_TRUE(sec.getGroupKey(GroupAddress(1, 2, 4), found).isOk());
    TEST_ASSERT_EQUAL_MEMORY(key.data(), found.data(), 16);
    TEST_ASSERT_FALSE(sec.getGroupKey(GroupAddress(1, 2, 3), found).isOk());

    const auto summary = sec.keyTableSummary();
    TEST_ASSERT_EQUAL_UINT16(1, summary.groupKeys);
    TEST_ASSERT_EQUAL_UINT16(1, summary.unresolvedGroupKeys);
    TEST_ASSERT_EQUAL_UINT16(1, sec.groupKeyCount());
}

// A sender outside the Security Individual Address Table takes an unlisted
// slot. When every slot is taken, one more such sender cannot be tracked and is
// refused; senders in the table, and those that already hold a slot, still are.
void test_SecurityInterfaceObject_UnlistedSendersAreBounded() {
    SecurityInterfaceObject sec;
    std::vector<uint8_t> table;
    appendIaEntry(table, IndividualAddress(2, 1, 1), 0u);
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, table).isOk());

    for (size_t i = 0; i < knx::config::SECURITY_UNLISTED_SENDERS; ++i) {
        const IndividualAddress sender(1, 1, static_cast<uint8_t>(i + 1u));
        TEST_ASSERT_TRUE(sec.canTrackPeer(sender));
        TEST_ASSERT_TRUE(sec.setPeerSequence(sender, 10u + i).isOk());
    }
    const IndividualAddress oneMore(1, 1, 250);
    TEST_ASSERT_FALSE(sec.canTrackPeer(oneMore));
    TEST_ASSERT_TRUE(sec.setPeerSequence(oneMore, 1u).isError());
    TEST_ASSERT_TRUE(sec.getPeerSequence(oneMore) == 0u);

    TEST_ASSERT_TRUE(sec.canTrackPeer(IndividualAddress(1, 1, 1)));
    TEST_ASSERT_TRUE(sec.setPeerSequence(IndividualAddress(1, 1, 1), 99u).isOk());
    TEST_ASSERT_TRUE(sec.getPeerSequence(IndividualAddress(1, 1, 1)) == 99u);
    TEST_ASSERT_TRUE(sec.setPeerSequence(IndividualAddress(2, 1, 1), 5u).isOk());
    TEST_ASSERT_TRUE(sec.getPeerSequence(IndividualAddress(2, 1, 1)) == 5u);
}

// Firmware keys are a fixed array too: one past KNX_SECURITY_FIRMWARE_KEYS is
// refused, and replacing an existing one takes no new slot.
void test_SecurityInterfaceObject_FirmwareKeysAreBounded() {
    SecurityInterfaceObject sec;
    std::array<uint8_t, 16> key{};
    key.fill(0x11);
    for (size_t i = 0; i < knx::config::SECURITY_FIRMWARE_KEYS; ++i) {
        sec.setGroupKey(GroupAddress(static_cast<uint16_t>(0x0800u + i)), key);
    }
    TEST_ASSERT_EQUAL_UINT16(knx::config::SECURITY_FIRMWARE_KEYS, sec.groupKeyCount());
    sec.setGroupKey(GroupAddress(0x0800u), key);
    TEST_ASSERT_EQUAL_UINT16(knx::config::SECURITY_FIRMWARE_KEYS, sec.groupKeyCount());
    sec.setDeviceKey(IndividualAddress(1, 1, 1), key);
    std::array<uint8_t, 16> found{};
    TEST_ASSERT_FALSE(sec.getDeviceKey(IndividualAddress(1, 1, 1), found).isOk());
}

// What ETS does in a secure download, and the secured traffic after it, with
// the heap as a device has it after start-up: nothing may allocate. Before the
// tables were fixed arrays every table write, the key resolution after it and
// the first telegram from each sender allocated.
void test_SecurityInterfaceObject_SecureDownloadAllocatesNothing() {
    SecurityInterfaceObject sec;
    sec.setGroupObjectCount(24);
    sec.setGroupAddressResolver(&addressAt, nullptr);
    std::array<uint8_t, 16> key{};
    key.fill(0x3C);

    std::vector<uint8_t> addresses;
    for (uint8_t device = 1; device <= 8; ++device) {
        appendIaEntry(addresses, IndividualAddress(1, 1, device), device);
    }
    std::vector<uint8_t> groupKeys;
    for (uint16_t index = 1; index <= std::size(kAddressTable); ++index) {
        appendKeyEntry(groupKeys, index, key, false);
    }
    std::vector<uint8_t> deviceKeys;
    appendKeyEntry(deviceKeys, 1, key, true);
    appendKeyEntry(deviceKeys, 2, key, true);
    std::vector<uint8_t> goFlags(24, 0x03);
    const uint8_t startLoading[] = {loadstate::kEventStartLoading};
    const uint8_t loadCompleted[] = {loadstate::kEventLoadCompleted};
    const auto pidLoad = static_cast<application::PropertyID>(SecurityProperty::LoadStateControl);
    const auto pidGoFlags = static_cast<application::PropertyID>(SecurityProperty::GoSecurityFlags);
    const auto binding = sec.kernelBinding();

    // The counter sees this program's allocations.
    g_allocations = 0;
    g_countAllocations = true;
    { auto probe = std::make_unique<std::array<uint8_t, 8>>(); (void)probe; }
    TEST_ASSERT_EQUAL_UINT32(1u, g_allocations);

    g_allocations = 0;
    for (int round = 0; round < 2; ++round) {  // a first download, then another
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, pidLoad, startLoading).isOk());
        TEST_ASSERT_TRUE(resizeProperty(binding.handlers, binding.handlerCount, binding.context,
                                        kPidIaTable, 1).isOk());
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, addresses).isOk());
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidGroupKeyTable, groupKeys).isOk());
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidP2PKeyTable, deviceKeys).isOk());
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, pidGoFlags, goFlags).isOk());
        TEST_ASSERT_TRUE(writeSecurityProperty(sec, pidLoad, loadCompleted).isOk());

        std::array<uint8_t, 16> found{};
        TEST_ASSERT_TRUE(sec.getGroupKey(GroupAddress(1, 2, 5), found).isOk());
        TEST_ASSERT_TRUE(sec.getDeviceKey(IndividualAddress(1, 1, 2), found).isOk());
        TEST_ASSERT_EQUAL_UINT16(3, sec.keyTableSummary().groupKeys);
        TEST_ASSERT_EQUAL_UINT8(0x03, sec.goSecurityFlags(23));
        // Every listed partner, and as many unlisted senders as there are slots.
        for (uint8_t device = 1; device <= 8; ++device) {
            TEST_ASSERT_TRUE(sec.setPeerSequence(IndividualAddress(1, 1, device), 1000u + device).isOk());
        }
        for (size_t i = 0; i < knx::config::SECURITY_UNLISTED_SENDERS; ++i) {
            (void)sec.setPeerSequence(IndividualAddress(3, 1, static_cast<uint8_t>(i + 1u)), 7u);
        }
        TEST_ASSERT_FALSE(sec.tableData(SecurityProperty::SecurityIndividualAddressTable).empty());
    }
    g_countAllocations = false;
    TEST_ASSERT_EQUAL_UINT32(0u, g_allocations);
    // The second download kept every partner's number.
    TEST_ASSERT_TRUE(sec.getPeerSequence(IndividualAddress(1, 1, 8)) == 1008u);
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
    std::vector<uint8_t> table;
    appendIaEntry(table, IndividualAddress(1, 1, 7), 0u);
    TEST_ASSERT_TRUE(writeSecurityProperty(sec, kPidIaTable, table).isOk());
    TEST_ASSERT_TRUE(sec.setPeerSequence(IndividualAddress(1, 1, 7), 10u).isOk());
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
    RUN_TEST(test_SecurityInterfaceObject_DownloadNeverMovesSequencesBackwards);
    RUN_TEST(test_SecurityInterfaceObject_TablesRefuseWritesPastTheirCapacity);
    RUN_TEST(test_SecurityInterfaceObject_GroupKeyResolvesThroughTheAddressTable);
    RUN_TEST(test_SecurityInterfaceObject_UnlistedSendersAreBounded);
    RUN_TEST(test_SecurityInterfaceObject_FirmwareKeysAreBounded);
    RUN_TEST(test_SecurityInterfaceObject_SecureDownloadAllocatesNothing);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveWritesOncePerWindow);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveExhaustedWritesBeforeSending);
    RUN_TEST(test_SecurityInterfaceObject_SendingReserveFailureRefusesTheNumber);
    RUN_TEST(test_SecurityInterfaceObject_RestoredCeilingRaisesTheCounter);
    RUN_TEST(test_SecurityInterfaceObject_CheckpointRecordsHaveSeparateFlags);

    return UNITY_END();
}
