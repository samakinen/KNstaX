// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_device_object.cpp
 * @brief Unit tests for DeviceObject
 */

#include "knx/objects/device_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/property_kernel.hpp"
#include <unity.h>
#include <vector>

using namespace knx;
using namespace knx::objects;

static DeviceObject* device = nullptr;

static util::Result<void> readDeviceProperty(const DeviceObject& obj,
                                             application::PropertyID id,
                                             uint16_t elementCount,
                                             std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeDeviceProperty(DeviceObject& obj,
                                              application::PropertyID id,
                                              std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

void setUp() {
    device = new DeviceObject();
}

void tearDown() {
    delete device;
    device = nullptr;
}

// === Basic Properties Tests ===

void test_DeviceObject_DefaultConstruction() {
    TEST_ASSERT_EQUAL_UINT16(0, device->getManufacturerId().value());
    TEST_ASSERT_EQUAL_UINT8(0, device->getFirmwareRevision());
    TEST_ASSERT_EQUAL_UINT16(254, device->getMaxApduLength()); // Default
    TEST_ASSERT_EQUAL(LoadState::Unloaded, device->getLoadState());
    TEST_ASSERT_EQUAL(RunState::Halted, device->getRunState());
    TEST_ASSERT_FALSE(device->getProgMode());
}

void test_DeviceObject_ManufacturerId() {
    device->setManufacturerId(ManufacturerId(0x1234));
    TEST_ASSERT_EQUAL_UINT16(0x1234, device->getManufacturerId().value());

    // Test via property access
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ManufacturerId),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x12, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, value[1]);
}

void test_DeviceObject_HardwareType() {
    std::vector<uint8_t> hwType = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    device->setHardwareType(std::span<const uint8_t>(hwType));
    
    const auto result = device->getHardwareType();
    TEST_ASSERT_EQUAL_INT(6, result.size());
    TEST_ASSERT_EQUAL_MEMORY(hwType.data(), result.data(), 6);
}

void test_DeviceObject_SerialNumber() {
    std::vector<uint8_t> serial = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    device->setSerialNumber(std::span<const uint8_t>(serial));
    
    const auto result = device->getSerialNumber();
    TEST_ASSERT_EQUAL_INT(6, result.size());
    TEST_ASSERT_EQUAL_MEMORY(serial.data(), result.data(), 6);
}

void test_DeviceObject_IndividualAddress() {
    IndividualAddress addr(1, 2, 3); // Area 1, Line 2, Device 3
    device->setIndividualAddress(addr);
    
    IndividualAddress result = device->getIndividualAddress();
    TEST_ASSERT_EQUAL_UINT16(addr.raw, result.raw);
}

void test_DeviceObject_MaxApduLength() {
    device->setMaxApduLength(255);
    TEST_ASSERT_EQUAL_UINT16(255, device->getMaxApduLength());
    
    device->setMaxApduLength(64);
    TEST_ASSERT_EQUAL_UINT16(64, device->getMaxApduLength());
}

void test_DeviceObject_RoutingCount() {
    device->setRoutingCount(7);
    TEST_ASSERT_EQUAL_UINT8(7, device->getRoutingCount());
    
    // Default is 6
    DeviceObject device2;
    TEST_ASSERT_EQUAL_UINT8(6, device2.getRoutingCount());
}

void test_DeviceObject_Strings() {
    device->setOrderInfo("TP1-123456");
    TEST_ASSERT_EQUAL_STRING("TP1-123456", device->getOrderInfo().c_str());
    
    device->setDescription("KNX Test Device");
    TEST_ASSERT_EQUAL_STRING("KNX Test Device", device->getDescription().c_str());
}

// === Load State Machine Tests ===

void test_DeviceObject_LoadState_InitialState() {
    TEST_ASSERT_EQUAL(LoadState::Unloaded, device->getLoadState());
    TEST_ASSERT_EQUAL(RunState::Halted, device->getRunState());
}

void test_DeviceObject_LoadState_StartLoad() {
    // Unloaded -> Loading
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_EQUAL(LoadState::Loading, device->getLoadState());
}

void test_DeviceObject_LoadState_LoadComplete() {
    // Unloaded -> Loading -> LoadCompleting -> Loaded
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_EQUAL(LoadState::Loading, device->getLoadState());
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_EQUAL(LoadState::LoadCompleting, device->getLoadState());
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_EQUAL(LoadState::Loaded, device->getLoadState());
    TEST_ASSERT_EQUAL(RunState::Ready, device->getRunState()); // Auto-transition
}

void test_DeviceObject_LoadState_Unload() {
    // Load first
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_EQUAL(LoadState::Loaded, device->getLoadState());
    
    // Then unload: Loaded -> Unloading -> Unloaded
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartUnload).isOk());
    TEST_ASSERT_EQUAL(LoadState::Unloading, device->getLoadState());
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::UnloadComplete).isOk());
    TEST_ASSERT_EQUAL(LoadState::Unloaded, device->getLoadState());
    TEST_ASSERT_EQUAL(RunState::Halted, device->getRunState()); // Auto-halt
}

void test_DeviceObject_LoadState_ErrorHandling() {
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_EQUAL(LoadState::Loading, device->getLoadState());
    
    // Error during load
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::Error).isOk());
    TEST_ASSERT_EQUAL(LoadState::Error, device->getLoadState());
    TEST_ASSERT_EQUAL(DeviceError::LoadError, device->getLastError());
}

void test_DeviceObject_LoadState_InvalidTransition() {
    // Try to unload when not loaded
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartUnload).isError());
    TEST_ASSERT_EQUAL(LoadState::Unloaded, device->getLoadState()); // No change
    
    // Try to load complete when not loading
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isError());
}

void test_DeviceObject_LoadState_Callback() {
    LoadState oldState = LoadState::Unloaded;
    LoadState newState = LoadState::Unloaded;
    
    device->registerLoadStateCallback([&](LoadState old, LoadState newSt) {
        oldState = old;
        newState = newSt;
    });
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_EQUAL(LoadState::Unloaded, oldState);
    TEST_ASSERT_EQUAL(LoadState::Loading, newState);
}

// === Programming Mode Tests ===

void test_DeviceObject_ProgMode_Default() {
    TEST_ASSERT_FALSE(device->getProgMode());
}

void test_DeviceObject_ProgMode_Enable() {
    device->setProgMode(knx::Toggle::Enable);
    TEST_ASSERT_TRUE(device->getProgMode());
    
    device->setProgMode(knx::Toggle::Disable);
    TEST_ASSERT_FALSE(device->getProgMode());
}

void test_DeviceObject_ProgMode_Callback() {
    bool callbackCalled = false;
    bool progModeState = false;
    
    device->registerProgModeCallback([&](knx::Toggle mode) {
        callbackCalled = true;
        progModeState = (mode == knx::Toggle::Enable);
    });
    
    device->setProgMode(knx::Toggle::Enable);
    TEST_ASSERT_TRUE(callbackCalled);
    TEST_ASSERT_TRUE(progModeState);
}

void test_DeviceObject_ProgMode_NoCallbackIfNoChange() {
    int callbackCount = 0;
    
    device->registerProgModeCallback([&](knx::Toggle) {
        callbackCount++;
    });
    
    device->setProgMode(knx::Toggle::Disable); // Already false
    TEST_ASSERT_EQUAL(0, callbackCount);
    
    device->setProgMode(knx::Toggle::Enable);
    TEST_ASSERT_EQUAL(1, callbackCount);
    
    device->setProgMode(knx::Toggle::Enable); // Already true
    TEST_ASSERT_EQUAL(1, callbackCount); // No additional callback
}

// === Property Access Tests ===

void test_DeviceObject_PropertyAccess_ManufacturerId() {
    device->setManufacturerId(ManufacturerId(0x1234));
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ManufacturerId),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x12, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, value[1]);

    value = {0x00, 0x01};
    TEST_ASSERT_FALSE(writeDeviceProperty(*device,
                                          static_cast<application::PropertyID>(DeviceProperty::ManufacturerId),
                                          value)
                          .isOk());
}

void test_DeviceObject_PropertyAccess_ProgMode() {
    std::vector<uint8_t> value = {0x01};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                         value)
                         .isOk());
    TEST_ASSERT_TRUE(device->getProgMode());
    
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x01, value[0]);
}

// === Programming Mode - Realisation Type 1 (property based) ===
//
// Profiles v02.01.01 §4.4.1.1 a) assigns this realisation to System B (mask
// 07B0), which is what this stack presents.  The memory-mapped variant at
// address 0060h is Realisation Type 2 and belongs to System 1/2, BCU 1/2 and
// BIM M112 — deliberately not implemented.  Resources v01.10.01 §4.3.5 defines
// the property; §4.26.3.3 requires the reported state to follow the internal
// one whatever changed it.

void test_DeviceObject_PropertyAccess_ProgModeWriteNotifiesObservers() {
    // Regression: this setter was once bound to setProgModeSilent(), so an ETS
    // write updated the stored bit while the BAU/application layer and the LED
    // kept the old state — the device stayed discoverable and lit after ETS
    // switched programming mode off.
    int progModeCallbacks = 0;
    bool observedProgMode = false;
    device->registerProgModeCallback([&](knx::Toggle mode) {
        ++progModeCallbacks;
        observedProgMode = (mode == knx::Toggle::Enable);
    });

    int ledCallbacks = 0;
    bool observedLed = false;
    device->setLedCallback([&](knx::Toggle state) {
        ++ledCallbacks;
        observedLed = (state == knx::Toggle::Enable);
    });

    std::vector<uint8_t> value = {0x01};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                         value)
                         .isOk());

    TEST_ASSERT_TRUE(device->getProgMode());
    TEST_ASSERT_EQUAL_INT(1, progModeCallbacks);
    TEST_ASSERT_TRUE(observedProgMode);
    TEST_ASSERT_EQUAL_INT(1, ledCallbacks);
    TEST_ASSERT_TRUE(observedLed);
}

void test_DeviceObject_PropertyAccess_ProgModeClearNotifiesObservers() {
    // The direction that actually failed in the field: ETS ending the
    // "Program individual address" procedure by switching programming mode off.
    device->setProgMode(knx::Toggle::Enable);

    int progModeCallbacks = 0;
    bool observedProgMode = true;
    device->registerProgModeCallback([&](knx::Toggle mode) {
        ++progModeCallbacks;
        observedProgMode = (mode == knx::Toggle::Enable);
    });

    int ledCallbacks = 0;
    bool observedLed = true;
    device->setLedCallback([&](knx::Toggle state) {
        ++ledCallbacks;
        observedLed = (state == knx::Toggle::Enable);
    });

    std::vector<uint8_t> value = {0x00};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                         value)
                         .isOk());

    TEST_ASSERT_FALSE(device->getProgMode());
    TEST_ASSERT_EQUAL_INT(1, progModeCallbacks);
    TEST_ASSERT_FALSE(observedProgMode);
    TEST_ASSERT_EQUAL_INT(1, ledCallbacks);
    TEST_ASSERT_FALSE(observedLed);

    // DMP_InterfaceObjectVerify_R re-reads the property after the write.
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
}

void test_DeviceObject_PropertyAccess_ProgModeReflectsLocalActivation() {
    // §4.26.3.3: "independent of the method by which the Programming Mode is
    // activated or deactivated" — a programming-button press must be visible to
    // a management client reading the property.
    device->setProgMode(knx::Toggle::Enable);

    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x01, value[0]);

    device->setProgMode(knx::Toggle::Disable);
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
}

void test_DeviceObject_PropertyAccess_ProgModeReservedBitsIgnored() {
    // §4.3.5: bit 0 carries the state; bits 1 to 7 are reserved and shall
    // always be 0.  Only bit 0 may be interpreted on write, and a read must not
    // leak anything into the reserved bits.
    std::vector<uint8_t> allButBitZero = {0xFE};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                         allButBitZero)
                         .isOk());
    TEST_ASSERT_FALSE(device->getProgMode());

    std::vector<uint8_t> reservedBitsSet = {0xFF};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                         reservedBitsSet)
                         .isOk());
    TEST_ASSERT_TRUE(device->getProgMode());

    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT8(0x01, value[0]);
}

void test_DeviceObject_ProgModeDescriptionMatchesSpec() {
    // Both DMP_InterfaceObjectWrite_R and DMP_InterfaceObjectVerify_R may open
    // with A_PropertyDescription_Read, so the description has to be right:
    // PDT_BITSET8, one element, writable.
    const auto binding = device->kernelBinding();
    application::PropertyID resolved{};
    application::PropertyDataType type{};
    uint16_t maxElements = 0;
    PropertyCapability capability{};

    TEST_ASSERT_TRUE(describeProperty(binding.handlers,
                                      binding.handlerCount,
                                      static_cast<application::PropertyID>(DeviceProperty::ProgMode),
                                      PropertyIndex(0),
                                      resolved,
                                      type,
                                      maxElements,
                                      capability)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(DeviceProperty::ProgMode),
                             static_cast<uint16_t>(resolved));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(application::PropertyDataType::Bitset8),
                            static_cast<uint8_t>(type));
    TEST_ASSERT_EQUAL_UINT16(1, maxElements);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PropertyCapability::ReadWrite),
                            static_cast<uint8_t>(capability));
}

void test_DeviceObject_ProgModeIsNotPersisted() {
    // §4.3.5: "The value shall be set to 0 on reset."  Persisting it once left
    // the device booting straight back into the Commissioning lifecycle with
    // group communication muted, so this guards the manifest entry.
    const auto entries = objectPropertyManifestEntries(InterfaceObjectType::device());
    bool found = false;
    for (const auto& entry : entries) {
        if (entry.registration.propertyId ==
            static_cast<application::PropertyID>(DeviceProperty::ProgMode)) {
            found = true;
            TEST_ASSERT_FALSE(isPersistedProperty(entry));
        }
    }
    TEST_ASSERT_TRUE(found);
}

void test_DeviceObject_PropertyAccess_LoadState() {
    // Load state is read-only via property access
    std::vector<uint8_t> value = {0x01};
    TEST_ASSERT_FALSE(writeDeviceProperty(*device,
                                          static_cast<application::PropertyID>(DeviceProperty::LoadStateControl),
                                          value)
                          .isOk());
    
    // But can read it
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::LoadStateControl),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LoadState::Unloaded), value[0]);
}

void test_DeviceObject_PropertyAccess_RunState() {
    std::vector<uint8_t> value = {static_cast<uint8_t>(RunState::Running)};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::RunStateControl),
                                         value)
                         .isOk());
    TEST_ASSERT_EQUAL(RunState::Running, device->getRunState());
    
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::RunStateControl),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(1, value.size());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RunState::Running), value[0]);
}

void test_DeviceObject_PropertyAccess_SubnetDeviceAddress() {
    device->setProgMode(knx::Toggle::Enable);
    std::vector<uint8_t> subnet = {0x11};
    std::vector<uint8_t> deviceAddr = {0x23};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::SubnetAddress),
                                         subnet)
                         .isOk());
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::DeviceAddress),
                                         deviceAddr)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(0x1123, device->getIndividualAddress().raw);
    device->setProgMode(knx::Toggle::Disable);
}

void test_DeviceObject_PropertyAccess_SubnetDeviceAddress_NotifiesAddressCallback() {
    device->setProgMode(knx::Toggle::Enable);

    int callbackCount = 0;
    IndividualAddress lastAddress;
    device->registerIndividualAddressCallback([&](const IndividualAddress& address) {
        ++callbackCount;
        lastAddress = address;
    });

    const std::vector<uint8_t> subnet = {0x11};
    const std::vector<uint8_t> deviceAddr = {0x23};
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::SubnetAddress),
                                         subnet)
                         .isOk());
    TEST_ASSERT_TRUE(writeDeviceProperty(*device,
                                         static_cast<application::PropertyID>(DeviceProperty::DeviceAddress),
                                         deviceAddr)
                         .isOk());

    TEST_ASSERT_EQUAL(2, callbackCount);
    TEST_ASSERT_EQUAL_UINT16(0x1123, lastAddress.raw);
    device->setProgMode(knx::Toggle::Disable);
}

void test_DeviceObject_PropertyAccess_InvalidProperty() {
    std::vector<uint8_t> value;
    TEST_ASSERT_FALSE(readDeviceProperty(*device,
                                         static_cast<application::PropertyID>(static_cast<DeviceProperty>(255)),
                                         1,
                                         value)
                          .isOk());
    value = {0x01};
    TEST_ASSERT_FALSE(writeDeviceProperty(*device,
                                          static_cast<application::PropertyID>(static_cast<DeviceProperty>(255)),
                                          value)
                          .isOk());
}

// === Error Handling Tests ===

void test_DeviceObject_ErrorHandling() {
    TEST_ASSERT_EQUAL(DeviceError::None, device->getLastError());
    
    device->setLastError(DeviceError::ConfigError);
    TEST_ASSERT_EQUAL(DeviceError::ConfigError, device->getLastError());
    
    device->clearError();
    TEST_ASSERT_EQUAL(DeviceError::None, device->getLastError());
    TEST_ASSERT_EQUAL_UINT8(0, device->getErrorFlags());
}

// === Validation Tests ===

void test_DeviceObject_IsValid_Default() {
    TEST_ASSERT_FALSE(device->isValid()); // No manufacturer ID or address
}

void test_DeviceObject_IsValid_WithConfig() {
    device->setManufacturerId(ManufacturerId(0x1234));
    device->setIndividualAddress(IndividualAddress(1, 1, 1));
    TEST_ASSERT_TRUE(device->isValid());
}

void test_DeviceObject_IsValid_WithError() {
    device->setManufacturerId(ManufacturerId(0x1234));
    device->setIndividualAddress(IndividualAddress(1, 1, 1));
    TEST_ASSERT_TRUE(device->isValid());
    
    device->setLastError(DeviceError::MemoryError);
    TEST_ASSERT_FALSE(device->isValid());
}

void test_DeviceObject_IsConfigured() {
    TEST_ASSERT_FALSE(device->isConfigured());
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::StartLoad).isOk());
    TEST_ASSERT_FALSE(device->isConfigured());
    
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_TRUE(device->processLoadEvent(LoadEvent::LoadComplete).isOk());
    TEST_ASSERT_TRUE(device->isConfigured());
}

void test_DeviceObject_IsRunning() {
    TEST_ASSERT_FALSE(device->isRunning());
    
    device->setRunState(RunState::Running);
    TEST_ASSERT_TRUE(device->isRunning());
    
    device->setRunState(RunState::Halted);
    TEST_ASSERT_FALSE(device->isRunning());
}

// PID_HARDWARE_TYPE gates whether ETS will download an application program
// into this hardware (03/05/01 §4.3.28). It is fixed at six octets with the
// MSB reserved as 00h, so a short value must be padded rather than truncated.
void test_DeviceObject_PropertyAccess_HardwareType() {
    const uint8_t hardwareType[6] = {0x00, 0x00, 0xFA, 0x01, 0x00, 0x01};
    device->setHardwareType(hardwareType);

    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::HardwareType),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(6, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);  // MSB shall be 00h
    TEST_ASSERT_EQUAL_UINT8(0xFA, value[2]);
    TEST_ASSERT_EQUAL_UINT8(0x01, value[5]);

    // Short value still answers with the full fixed width.
    const uint8_t shortType[2] = {0x00, 0x07};
    device->setHardwareType(shortType);
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::HardwareType),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(6, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x07, value[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, value[5]);

    // Read-only: the hardware cannot be rewritten over the bus.
    std::vector<uint8_t> attempt = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    TEST_ASSERT_FALSE(writeDeviceProperty(*device,
                                          static_cast<application::PropertyID>(DeviceProperty::HardwareType),
                                          attempt)
                          .isOk());
}

// PID_ORDER_INFO: PDT_GENERIC_10, zero-padded (03/05/01 §4.2.15).
void test_DeviceObject_PropertyAccess_OrderInfo() {
    device->setOrderInfo("SBTP1");

    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::OrderInfo),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(10, value.size());
    TEST_ASSERT_EQUAL_UINT8('S', value[0]);
    TEST_ASSERT_EQUAL_UINT8('1', value[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, value[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, value[9]);

    // Longer than the fixed width is truncated, not overflowed.
    device->setOrderInfo("ORDERINFO-TOO-LONG");
    value.clear();
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::OrderInfo),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(10, value.size());
    TEST_ASSERT_EQUAL_UINT8('O', value[0]);
    TEST_ASSERT_EQUAL_UINT8('O', value[8]);   // "ORDERINFO" is the first 9
    TEST_ASSERT_EQUAL_UINT8('-', value[9]);   // cut cleanly at 10 octets
}

// PID_VERSION encoded as DPT_Version (217.001): U5 magic, U5 version, U6
// revision, big-endian across two octets.
void test_DeviceObject_PropertyAccess_Version() {
    device->setVersion(0, 1, 1);
    TEST_ASSERT_EQUAL_UINT16((1u << 6) | 1u, device->getVersion());

    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readDeviceProperty(*device,
                                        static_cast<application::PropertyID>(DeviceProperty::Version),
                                        1,
                                        value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x41, value[1]);

    // Each field occupies its own bit range and saturates at its width.
    TEST_ASSERT_EQUAL_UINT16(0xFFFFu & ((31u << 11) | (31u << 6) | 63u),
                             DeviceObject::packVersion(31, 31, 63));
    // Over-wide inputs are masked, never allowed to bleed into a neighbour.
    TEST_ASSERT_EQUAL_UINT16(DeviceObject::packVersion(0, 0, 0),
                             DeviceObject::packVersion(32, 32, 64));
}

// === Main Test Runner ===

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    // Basic properties
    RUN_TEST(test_DeviceObject_DefaultConstruction);
    RUN_TEST(test_DeviceObject_ManufacturerId);
    RUN_TEST(test_DeviceObject_PropertyAccess_HardwareType);
    RUN_TEST(test_DeviceObject_PropertyAccess_OrderInfo);
    RUN_TEST(test_DeviceObject_PropertyAccess_Version);
    RUN_TEST(test_DeviceObject_HardwareType);
    RUN_TEST(test_DeviceObject_SerialNumber);
    RUN_TEST(test_DeviceObject_IndividualAddress);
    RUN_TEST(test_DeviceObject_MaxApduLength);
    RUN_TEST(test_DeviceObject_RoutingCount);
    RUN_TEST(test_DeviceObject_Strings);
    
    // Load state machine
    RUN_TEST(test_DeviceObject_LoadState_InitialState);
    RUN_TEST(test_DeviceObject_LoadState_StartLoad);
    RUN_TEST(test_DeviceObject_LoadState_LoadComplete);
    RUN_TEST(test_DeviceObject_LoadState_Unload);
    RUN_TEST(test_DeviceObject_LoadState_ErrorHandling);
    RUN_TEST(test_DeviceObject_LoadState_InvalidTransition);
    RUN_TEST(test_DeviceObject_LoadState_Callback);
    
    // Programming mode
    RUN_TEST(test_DeviceObject_ProgMode_Default);
    RUN_TEST(test_DeviceObject_ProgMode_Enable);
    RUN_TEST(test_DeviceObject_ProgMode_Callback);
    RUN_TEST(test_DeviceObject_ProgMode_NoCallbackIfNoChange);
    
    // Property access
    RUN_TEST(test_DeviceObject_PropertyAccess_ManufacturerId);
    RUN_TEST(test_DeviceObject_PropertyAccess_ProgMode);
    RUN_TEST(test_DeviceObject_PropertyAccess_ProgModeWriteNotifiesObservers);
    RUN_TEST(test_DeviceObject_PropertyAccess_ProgModeClearNotifiesObservers);
    RUN_TEST(test_DeviceObject_PropertyAccess_ProgModeReflectsLocalActivation);
    RUN_TEST(test_DeviceObject_PropertyAccess_ProgModeReservedBitsIgnored);
    RUN_TEST(test_DeviceObject_ProgModeDescriptionMatchesSpec);
    RUN_TEST(test_DeviceObject_ProgModeIsNotPersisted);
    RUN_TEST(test_DeviceObject_PropertyAccess_LoadState);
    RUN_TEST(test_DeviceObject_PropertyAccess_RunState);
    RUN_TEST(test_DeviceObject_PropertyAccess_SubnetDeviceAddress);
    RUN_TEST(test_DeviceObject_PropertyAccess_SubnetDeviceAddress_NotifiesAddressCallback);
    RUN_TEST(test_DeviceObject_PropertyAccess_InvalidProperty);
    
    // Error handling
    RUN_TEST(test_DeviceObject_ErrorHandling);
    
    // Validation
    RUN_TEST(test_DeviceObject_IsValid_Default);
    RUN_TEST(test_DeviceObject_IsValid_WithConfig);
    RUN_TEST(test_DeviceObject_IsValid_WithError);
    RUN_TEST(test_DeviceObject_IsConfigured);
    RUN_TEST(test_DeviceObject_IsRunning);
    
    return UNITY_END();
}
