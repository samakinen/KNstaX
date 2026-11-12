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
// describeProperty() with its result spread over out-parameters, the shape
// these tests were written against. access: bit 0 read, bit 1 write.
[[maybe_unused]] knx::objects::PropertyAccessResult describeInto(
    knx::objects::InterfaceObjectManager& manager,
    knx::InterfaceObjectType objectType,
    knx::InterfaceObjectInstance objectInstance,
    knx::application::PropertyID propertyId,
    knx::PropertyIndex propertyIndex,
    knx::application::PropertyID& resolvedPropertyId,
    knx::application::PropertyDataType& type,
    uint16_t& maxElements,
    uint8_t& access,
    uint8_t& readLevel,
    uint8_t& writeLevel)
{
    knx::objects::PropertyDescription description;
    const auto res = manager.describeProperty(objectType, objectInstance, propertyId,
                                              propertyIndex.value(), description);
    resolvedPropertyId = description.id;
    type = description.type;
    maxElements = description.maxElements;
    access = static_cast<uint8_t>((knx::objects::canRead(description.capability) ? 0x01u : 0u)
                                  | (knx::objects::canWrite(description.capability) ? 0x02u : 0u));
    readLevel = description.readLevel;
    writeLevel = description.writeLevel;
    return res;
}
} // namespace

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
                                uint16_t propertyIndex,
                                PropertyDescription& description) {
            auto& resolvedPropertyId = description.id;
            auto& type = description.type;
            auto& maxElements = description.maxElements;
            auto& readLevel = description.readLevel;
            auto& writeLevel = description.writeLevel;
            (void)instance;

            // By PID when it is not zero, otherwise by 0-based index.
            if (static_cast<uint8_t>(propertyId) == 0) {
                if (propertyIndex == 0) {
                    resolvedPropertyId = static_cast<knx::application::PropertyID>(PROP_OBJECT_TYPE);
                } else if (propertyIndex == 1) {
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
                description.capability = PropertyCapability::ReadOnly;
                return PropertyAccessResult::Success;
            }

            if (static_cast<uint8_t>(resolvedPropertyId) == PROP_VALUE) {
                type = knx::application::PropertyDataType::GenericData;
                description.capability = PropertyCapability::ReadWrite;
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
                      describeInto(mgr, InterfaceObjectType(OBJTYPE_CUSTOM),
                                          InterfaceObjectInstance(1),
                                          static_cast<knx::application::PropertyID>(0),
                                          PropertyIndex(1),  // 0-based: the second property
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
                            uint16_t /*propertyIndex*/,
                            PropertyDescription& description) {
        description.id = propertyId;
        description.type = knx::application::PropertyDataType::GenericData;
        description.maxElements = 1;
        description.capability = PropertyCapability::ReadWrite;
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
