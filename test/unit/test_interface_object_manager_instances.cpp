// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_interface_object_manager_instances.cpp
 * @brief Unit tests for InterfaceObjectManager multi-instance routing
 */

#include "knx/objects/interface_object_manager.hpp"

#include <unity.h>

#include <cstdint>
#include <map>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::objects;

namespace {
constexpr uint16_t OBJTYPE_CUSTOM = 200;
constexpr uint8_t PROP_OBJECT_TYPE = 1;
constexpr uint8_t PROP_VALUE = 10;

struct CustomInstanceState {
    std::vector<uint8_t> value;
};
} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_InterfaceObjectManager_MultiInstance_ReadWriteAndDescribeRoutedByInstance(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    std::map<uint8_t, CustomInstanceState> state;
    state[1].value = {0x01};
    state[2].value = {0x02};

    auto makeHandlers = [&](uint8_t instance) -> InterfaceObjectManager::RegisteredObjectHandlers {
        InterfaceObjectManager::RegisteredObjectHandlers h;

        h.read = [&, instance](knx::application::PropertyID propertyId, uint16_t startIndex, uint8_t elementCount, knx::application::PropertyServiceDataBuffer& out) {
            if (startIndex != 1 || elementCount != 1) {
                return PropertyAccessResult::InvalidValue;
            }
            if (static_cast<uint8_t>(propertyId) == PROP_OBJECT_TYPE) {
                out = {static_cast<uint8_t>((OBJTYPE_CUSTOM >> 8) & 0xFF), static_cast<uint8_t>(OBJTYPE_CUSTOM & 0xFF)};
                return PropertyAccessResult::Success;
            }
            if (static_cast<uint8_t>(propertyId) == PROP_VALUE) {
                (void)out.assign(state[instance].value);
                return PropertyAccessResult::Success;
            }
            return PropertyAccessResult::InvalidProperty;
        };

        h.write = [&, instance](knx::application::PropertyID propertyId, uint16_t startIndex, std::span<const uint8_t> in) {
            if (startIndex != 1) {
                return PropertyAccessResult::InvalidValue;
            }
            if (static_cast<uint8_t>(propertyId) == PROP_VALUE) {
                state[instance].value.assign(in.begin(), in.end());
                return PropertyAccessResult::Success;
            }
            return PropertyAccessResult::InvalidProperty;
        };

        h.describe = [instance](knx::application::PropertyID propertyId,
                 PropertyIndex propertyIndex,
                         knx::application::PropertyID& resolvedPropertyId,
                         knx::application::PropertyDataType& type,
                         uint16_t& maxElements,
                         uint8_t& access,
                         uint8_t& readLevel,
                         uint8_t& writeLevel) {
            (void)instance;
            // access bits: bit0=read, bit1=write
            constexpr uint8_t ACCESS_READ = 0x01;
            constexpr uint8_t ACCESS_WRITE = 0x02;

            if (propertyIndex.value() != 0) {
                if (propertyIndex.value() == 1) {
                    resolvedPropertyId = static_cast<knx::application::PropertyID>(PROP_OBJECT_TYPE);
                } else if (propertyIndex.value() == 2) {
                    resolvedPropertyId = static_cast<knx::application::PropertyID>(PROP_VALUE);
                } else {
                    return PropertyAccessResult::InvalidProperty;
                }
            } else {
                resolvedPropertyId = propertyId;
            }

            readLevel = 0;
            writeLevel = 0;
            maxElements = 1;

            if (static_cast<uint8_t>(resolvedPropertyId) == PROP_OBJECT_TYPE) {
                type = knx::application::PropertyDataType::UnsignedInt;
                access = ACCESS_READ;
                return PropertyAccessResult::Success;
            }

            if (static_cast<uint8_t>(resolvedPropertyId) == PROP_VALUE) {
                type = knx::application::PropertyDataType::GenericData;
                access = ACCESS_READ | ACCESS_WRITE;
                return PropertyAccessResult::Success;
            }

            return PropertyAccessResult::InvalidProperty;
        };

        return h;
    };

    TEST_ASSERT_TRUE(mgr.registerObjectInstance(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(1), makeHandlers(1)).isOk());
    TEST_ASSERT_TRUE(mgr.registerObjectInstance(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(2), makeHandlers(2)).isOk());

    TEST_ASSERT_EQUAL_UINT8(2, mgr.getObjectCount(InterfaceObjectType(OBJTYPE_CUSTOM)));

    // Instance-specific read
    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(1),
                                       static_cast<knx::application::PropertyID>(PROP_VALUE), 1, 1, out));
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    if (out.size() != 1) {
        return;
    }
    TEST_ASSERT_EQUAL_HEX8(0x01, out[0]);

    out.clear();
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(2),
                                       static_cast<knx::application::PropertyID>(PROP_VALUE), 1, 1, out));
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    if (out.size() != 1) {
        return;
    }
    TEST_ASSERT_EQUAL_HEX8(0x02, out[0]);

    // Instance-specific write
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(2),
                                        static_cast<knx::application::PropertyID>(PROP_VALUE), 1, {0xAA}));
    out.clear();
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(2),
                                       static_cast<knx::application::PropertyID>(PROP_VALUE), 1, 1, out));
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    if (out.size() != 1) {
        return;
    }
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[0]);

    // Describe property by index should resolve property IDs.
    knx::application::PropertyID resolvedId = static_cast<knx::application::PropertyID>(0);
    knx::application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.describeProperty(InterfaceObjectType(OBJTYPE_CUSTOM),
                                          InterfaceObjectInstance(1),
                                          static_cast<knx::application::PropertyID>(0),
                                          PropertyIndex(2),
                                          resolvedId,
                                          type,
                                          maxElements,
                                          access,
                                          readLevel,
                                          writeLevel));
    TEST_ASSERT_EQUAL_UINT8(PROP_VALUE, static_cast<uint8_t>(resolvedId));

    // "All/unspecified" instance should be rejected when multiple instances exist.
    out.clear();
    TEST_ASSERT_EQUAL(PropertyAccessResult::InvalidObject,
                      mgr.readProperty(InterfaceObjectType(OBJTYPE_CUSTOM), InterfaceObjectInstance(0),
                                       static_cast<knx::application::PropertyID>(PROP_VALUE), 1, 1, out));
}

void test_InterfaceObjectManager_MixedBuiltInAndRegisteredInstance_CountAndRouting(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    std::vector<uint8_t> registeredValue{0xAB};

    InterfaceObjectManager::RegisteredObjectHandlers handlers;
    handlers.read = [&](knx::application::PropertyID propertyId, uint16_t startIndex, uint8_t elementCount, knx::application::PropertyServiceDataBuffer& out) {
        if (startIndex != 1 || elementCount != 1) {
            return PropertyAccessResult::InvalidValue;
        }
        if (static_cast<uint8_t>(propertyId) == PROP_VALUE) {
            (void)out.assign(registeredValue);
            return PropertyAccessResult::Success;
        }
        return PropertyAccessResult::InvalidProperty;
    };

    handlers.write = [&](knx::application::PropertyID propertyId, uint16_t startIndex, std::span<const uint8_t> in) {
        if (startIndex != 1) {
            return PropertyAccessResult::InvalidValue;
        }
        if (static_cast<uint8_t>(propertyId) == PROP_VALUE) {
            registeredValue.assign(in.begin(), in.end());
            return PropertyAccessResult::Success;
        }
        return PropertyAccessResult::InvalidProperty;
    };

    handlers.describe = [&](knx::application::PropertyID propertyId,
                            PropertyIndex propertyIndex,
                            knx::application::PropertyID& resolvedPropertyId,
                            knx::application::PropertyDataType& type,
                            uint16_t& maxElements,
                            uint8_t& access,
                            uint8_t& readLevel,
                            uint8_t& writeLevel) {
        (void)propertyIndex;
        resolvedPropertyId = propertyId;
        type = knx::application::PropertyDataType::GenericData;
        maxElements = 1;
        access = 0x03;
        readLevel = 0;
        writeLevel = 0;
        return PropertyAccessResult::Success;
    };

    TEST_ASSERT_TRUE(mgr.registerObjectInstance(InterfaceObjectType::addressTable(), InterfaceObjectInstance(2), std::move(handlers)).isOk());

    // Built-in singleton (instance 1) + one registered instance (instance 2).
    TEST_ASSERT_EQUAL_UINT8(2, mgr.getObjectCount(InterfaceObjectType::addressTable()));

    // Instance 1 remains routed to built-in address table object.
    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::addressTable(),
                                       InterfaceObjectInstance(1),
                                       static_cast<knx::application::PropertyID>(AddressTableProperty::ObjectType),
                                       1,
                                       1,
                                       out));
    TEST_ASSERT_EQUAL_UINT32(2, out.size());

    // Instance 2 routes to registered handlers.
    out.clear();
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.readProperty(InterfaceObjectType::addressTable(),
                                       InterfaceObjectInstance(2),
                                       static_cast<knx::application::PropertyID>(PROP_VALUE),
                                       1,
                                       1,
                                       out));
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    if (out.size() != 1) {
        return;
    }
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[0]);

    // Unspecified instance is ambiguous when both are reachable.
    out.clear();
    TEST_ASSERT_EQUAL(PropertyAccessResult::InvalidObject,
                      mgr.readProperty(InterfaceObjectType::addressTable(),
                                       InterfaceObjectInstance(0),
                                       static_cast<knx::application::PropertyID>(AddressTableProperty::ObjectType),
                                       1,
                                       1,
                                       out));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_InterfaceObjectManager_MultiInstance_ReadWriteAndDescribeRoutedByInstance);
    RUN_TEST(test_InterfaceObjectManager_MixedBuiltInAndRegisteredInstance_CountAndRouting);

    return UNITY_END();
}
