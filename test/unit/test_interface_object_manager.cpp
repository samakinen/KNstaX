// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_interface_object_manager.cpp
 * @brief Unit tests for InterfaceObjectManager property dispatch
 */

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/security_interface_object.hpp"
#include <unity.h>
#include <vector>

using namespace knx;
using namespace knx::objects;

void setUp(void) {}
void tearDown(void) {}

void test_InterfaceObjectManager_DescribeProperty_DeviceObject(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    knx::application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    knx::application::PropertyID resolvedPropertyId = static_cast<knx::application::PropertyID>(0);
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    auto res = mgr.describeProperty(
        InterfaceObjectType::device(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(DeviceProperty::SubnetAddress),
        PropertyIndex(0),
        resolvedPropertyId,
        type,
        maxElements,
        access,
        readLevel,
        writeLevel);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, res);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceProperty::SubnetAddress), static_cast<uint8_t>(resolvedPropertyId));
    TEST_ASSERT_EQUAL(knx::application::PropertyDataType::UnsignedChar, type);
    TEST_ASSERT_EQUAL_UINT16(1, maxElements);
    TEST_ASSERT_EQUAL_UINT8(0x03, access); // read+write
}

void test_InterfaceObjectManager_DescribeProperty_AddressTable_TableData(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    knx::application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    knx::application::PropertyID resolvedPropertyId = static_cast<knx::application::PropertyID>(0);
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    auto res = mgr.describeProperty(
        InterfaceObjectType::addressTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AddressTableProperty::TableData),
        PropertyIndex(0),
        resolvedPropertyId,
        type,
        maxElements,
        access,
        readLevel,
        writeLevel);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, res);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(AddressTableProperty::TableData),
                            static_cast<uint8_t>(resolvedPropertyId));
    TEST_ASSERT_EQUAL(knx::application::PropertyDataType::UnsignedInt, type);
    TEST_ASSERT_EQUAL_UINT16(256, maxElements);
    TEST_ASSERT_EQUAL_UINT8(0x03, access); // read+write
}

void test_InterfaceObjectManager_ReadWrite_AddressTable_TableDataSlice(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    // Write two group addresses at indices 1..2
    std::vector<uint8_t> writeBytes = {
        0x00, 0x11, // GA 0x0011
        0x00, 0x22  // GA 0x0022
    };

    auto wres = mgr.writeProperty(
        InterfaceObjectType::addressTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AddressTableProperty::TableData),
        1,
        writeBytes);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    knx::application::PropertyServiceDataBuffer readBytes;
    auto rres = mgr.readProperty(
        InterfaceObjectType::addressTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AddressTableProperty::TableData),
        1,
        2,
        readBytes);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(writeBytes.size(), readBytes.size());
    TEST_ASSERT_EQUAL_MEMORY(writeBytes.data(), readBytes.data(), writeBytes.size());
}

void test_InterfaceObjectManager_ReadWrite_AssociationTable_TableDataSlice(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    // One entry at index 1: addrIndex=1, goNum=0x1234
    std::vector<uint8_t> writeBytes = {
        0x00, 0x01,
        0x12, 0x34,
    };

    auto wres = mgr.writeProperty(
        InterfaceObjectType::associationTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData),
        1,
        writeBytes);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    knx::application::PropertyServiceDataBuffer readBytes;
    auto rres = mgr.readProperty(
        InterfaceObjectType::associationTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData),
        1,
        1,
        readBytes);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(writeBytes.size(), readBytes.size());
    TEST_ASSERT_EQUAL_MEMORY(writeBytes.data(), readBytes.data(), writeBytes.size());
}

void test_InterfaceObjectManager_ZeroElementCountRejected(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    knx::application::PropertyServiceDataBuffer out;
    auto rres = mgr.readProperty(
        InterfaceObjectType::addressTable(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(AddressTableProperty::TableData),
        1,
        0,
        out);

    TEST_ASSERT_EQUAL(PropertyAccessResult::InvalidValue, rres);
}

// Array element 0 is not an out-of-range element index: 03/03/07 §3.4.4.1 makes
// it the current number of elements, and 03/05/01 distinguishes that from the
// maximum the descriptor reports. ETS reads it to find out how much of a table
// is in use before downloading over it.
void test_InterfaceObjectManager_ElementZeroReadsCurrentElementCount(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto tableData = static_cast<knx::application::PropertyID>(AddressTableProperty::TableData);

    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::addressTable(),
                                       InterfaceObjectInstance(1), tableData, 0, 1, out));
    TEST_ASSERT_EQUAL_UINT32(2u, out.size());
    TEST_ASSERT_EQUAL_UINT16(0u, static_cast<uint16_t>((out[0] << 8) | out[1]));

    // Two group addresses downloaded; the count follows the table, the
    // descriptor's maximum does not move.
    const std::vector<uint8_t> addresses = {0x09, 0x02, 0x09, 0x03};
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType::addressTable(),
                                        InterfaceObjectInstance(1), tableData, 1, addresses));

    out = {};
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::addressTable(),
                                       InterfaceObjectInstance(1), tableData, 0, 1, out));
    TEST_ASSERT_EQUAL_UINT32(2u, out.size());
    TEST_ASSERT_EQUAL_UINT16(2u, static_cast<uint16_t>((out[0] << 8) | out[1]));
}

// ETS clears an array property it is about to re-download by writing the
// element count: "OT=17 PID=54 nr_of_elem=1 start_index=0 data=0000" is
// "empty the Security Individual Address Table". Refusing that write left a
// shrinking table with entries from the previous download in its tail.
void test_InterfaceObjectManager_ElementZeroWriteSetsElementCount(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto iaTable = static_cast<knx::application::PropertyID>(
        knx::objects::SecurityProperty::SecurityIndividualAddressTable);

    // Two 8-octet entries.
    std::vector<uint8_t> table(16, 0xAB);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType::security(),
                                        InterfaceObjectInstance(1), iaTable, 1, table));

    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::security(),
                                       InterfaceObjectInstance(1), iaTable, 0, 1, out));
    TEST_ASSERT_EQUAL_UINT16(2u, static_cast<uint16_t>((out[0] << 8) | out[1]));

    const std::vector<uint8_t> clear = {0x00, 0x00};
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType::security(),
                                        InterfaceObjectInstance(1), iaTable, 0, clear));

    out = {};
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::security(),
                                       InterfaceObjectInstance(1), iaTable, 0, 1, out));
    TEST_ASSERT_EQUAL_UINT16(0u, static_cast<uint16_t>((out[0] << 8) | out[1]));

    // A property whose length the firmware owns keeps refusing the write: the
    // address table's length follows its entries, not a client's declaration.
    const std::vector<uint8_t> shrink = {0x00, 0x01};
    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(PropertyAccessResult::Success),
        static_cast<int>(mgr.writeProperty(
            InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
            static_cast<knx::application::PropertyID>(AddressTableProperty::TableData), 0, shrink)));
}

// --- KNXnet/IP Parameter Object: PID 90-94 (Secure PIDs) ---

// PID values from the reference registry (DIN EN ISO 22510:2021 §5.7.2.5)
static constexpr knx::application::PropertyID kPidTunnellingUsers   = static_cast<knx::application::PropertyID>(90);
static constexpr knx::application::PropertyID kPidBackboneKey       = static_cast<knx::application::PropertyID>(91);
static constexpr knx::application::PropertyID kPidMulticastLatency  = static_cast<knx::application::PropertyID>(92);
static constexpr knx::application::PropertyID kPidSyncLatencyFrac   = static_cast<knx::application::PropertyID>(93);
static constexpr knx::application::PropertyID kPidSecuredSvcFamilies = static_cast<knx::application::PropertyID>(94);

void test_KnxNetIpParam_BackboneKey_Describe(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    knx::application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    knx::application::PropertyID resolvedId = static_cast<knx::application::PropertyID>(0);
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    auto res = mgr.describeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidBackboneKey,
        knx::PropertyIndex(0),
        resolvedId,
        type,
        maxElements,
        access,
        readLevel,
        writeLevel);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, res);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kPidBackboneKey), static_cast<uint8_t>(resolvedId));
    // PID 91 is Generic16 (1 element × 16 bytes), ReadWrite
    TEST_ASSERT_EQUAL(knx::application::PropertyDataType::Generic16, type);
    TEST_ASSERT_EQUAL_UINT16(1, maxElements);
    TEST_ASSERT_EQUAL_UINT8(0x03, access); // ReadWrite
}

void test_KnxNetIpParam_BackboneKey_WriteRead(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    // Write a 16-byte AES key
    const std::vector<uint8_t> key = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F
    };
    auto wres = mgr.writeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidBackboneKey,
        1, key);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    // Read it back
    knx::application::PropertyServiceDataBuffer buf;
    auto rres = mgr.readProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidBackboneKey,
        1, 1, buf);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(buf.size()));
    TEST_ASSERT_EQUAL_MEMORY(key.data(), buf.data(), 16);
}

void test_KnxNetIpParam_MulticastLatencyTolerance_WriteRead(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    // PID 92: UnsignedInt (2 bytes) — write 2000 ms (0x07D0)
    const std::vector<uint8_t> latency = {0x07, 0xD0};
    auto wres = mgr.writeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidMulticastLatency,
        1, latency);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    knx::application::PropertyServiceDataBuffer buf;
    auto rres = mgr.readProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidMulticastLatency,
        1, 1, buf);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(buf.size()));
    TEST_ASSERT_EQUAL_HEX8(0x07, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD0, buf[1]);
}

void test_KnxNetIpParam_SyncLatencyFraction_WriteRead(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    // PID 93: UnsignedChar (1 byte) — write 10 (%)
    const std::vector<uint8_t> fraction = {0x0A};
    auto wres = mgr.writeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidSyncLatencyFrac,
        1, fraction);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    knx::application::PropertyServiceDataBuffer buf;
    auto rres = mgr.readProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidSyncLatencyFrac,
        1, 1, buf);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(buf.size()));
    TEST_ASSERT_EQUAL_HEX8(0x0A, buf[0]);
}

void test_KnxNetIpParam_SecuredServiceFamilies_WriteRead(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    // PID 94: GenericData (1 byte/element) — write two elements:
    //   element 1 = 0x02 (family: tunnelling), element 2 = 0x01 (version: secure required)
    const std::vector<uint8_t> entry = {0x02, 0x01};
    auto wres = mgr.writeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidSecuredSvcFamilies,
        1, entry); // startIndex=1, writes 2 elements
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, wres);

    knx::application::PropertyServiceDataBuffer buf;
    auto rres = mgr.readProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidSecuredSvcFamilies,
        1, 2, buf); // startIndex=1, elementCount=2
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, rres);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(buf.size()));
    TEST_ASSERT_EQUAL_HEX8(0x02, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[1]);
}

void test_KnxNetIpParam_TunnellingUsers_Describe(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    knx::application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    knx::application::PropertyID resolvedId = static_cast<knx::application::PropertyID>(0);
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    auto res = mgr.describeProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        kPidTunnellingUsers,
        knx::PropertyIndex(0),
        resolvedId,
        type,
        maxElements,
        access,
        readLevel,
        writeLevel);

    // PID 90 must be declared (ReadWrite, GenericData, multi-element)
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success, res);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kPidTunnellingUsers), static_cast<uint8_t>(resolvedId));
    TEST_ASSERT_EQUAL_UINT8(0x03, access); // ReadWrite
}

void test_KnxNetIpParam_UndeclaredPid_RejectsInvalid(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    knx::application::PropertyServiceDataBuffer buf;
    // PID 95 is not declared in the knxNetIpParameter manifest
    auto rres = mgr.readProperty(
        InterfaceObjectType::knxNetIpParameter(),
        InterfaceObjectInstance(1),
        static_cast<knx::application::PropertyID>(95),
        1, 1, buf);

    TEST_ASSERT_NOT_EQUAL(
        static_cast<int>(PropertyAccessResult::Success),
        static_cast<int>(rres));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_InterfaceObjectManager_DescribeProperty_DeviceObject);
    RUN_TEST(test_InterfaceObjectManager_DescribeProperty_AddressTable_TableData);
    RUN_TEST(test_InterfaceObjectManager_ReadWrite_AddressTable_TableDataSlice);
    RUN_TEST(test_InterfaceObjectManager_ReadWrite_AssociationTable_TableDataSlice);
    RUN_TEST(test_InterfaceObjectManager_ZeroElementCountRejected);
    RUN_TEST(test_InterfaceObjectManager_ElementZeroReadsCurrentElementCount);
    RUN_TEST(test_InterfaceObjectManager_ElementZeroWriteSetsElementCount);
    RUN_TEST(test_KnxNetIpParam_BackboneKey_Describe);
    RUN_TEST(test_KnxNetIpParam_BackboneKey_WriteRead);
    RUN_TEST(test_KnxNetIpParam_MulticastLatencyTolerance_WriteRead);
    RUN_TEST(test_KnxNetIpParam_SyncLatencyFraction_WriteRead);
    RUN_TEST(test_KnxNetIpParam_SecuredServiceFamilies_WriteRead);
    RUN_TEST(test_KnxNetIpParam_TunnellingUsers_Describe);
    RUN_TEST(test_KnxNetIpParam_UndeclaredPid_RejectsInvalid);

    return UNITY_END();
}
