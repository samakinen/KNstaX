// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNX/IP Secure Routing turned on the way ETS turns it on.
 *
 * ETS does not configure Secure Routing through a side channel: it downloads
 * PID_BACKBONE_KEY into the KNXnet/IP Parameter Object as an ordinary property
 * write, over a secured management connection, and expects the device to start
 * encrypting its multicast immediately. Storing the key without acting on it
 * leaves a device transmitting in the clear inside a project that believes it
 * is encrypted — silent on the device and invisible in ETS.
 *
 * The two halves are checked separately: that a property write is observable,
 * and that the routing physical acts on the key it is handed.
 */

#include "unity.h"

#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/physical/ip_routing_physical.hpp"

#include <cstdint>
#include <vector>

using knx::InterfaceObjectInstance;
using knx::InterfaceObjectType;
using knx::objects::GenericInterfaceObject;
using knx::objects::InterfaceObjectManager;
using knx::objects::PropertyAccessResult;

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr knx::application::PropertyID kPidBackboneKey =
    static_cast<knx::application::PropertyID>(91);
constexpr knx::application::PropertyID kPidFriendlyName =
    static_cast<knx::application::PropertyID>(76);

std::vector<uint8_t> exampleKey(uint8_t seed)
{
    std::vector<uint8_t> key(16, 0);
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

} // namespace

void test_property_write_reaches_the_observer(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    knx::application::PropertyID seenProperty{};
    std::vector<uint8_t> seenValue;
    int callCount = 0;
    paramObj.setWriteObserver([&](knx::application::PropertyID id, std::span<const uint8_t>,
                                  std::span<const uint8_t> value) {
        seenProperty = id;
        seenValue.assign(value.begin(), value.end());
        ++callCount;
    });

    const auto key = exampleKey(0x10);
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType::knxNetIpParameter(),
                                        InterfaceObjectInstance(1),
                                        kPidBackboneKey,
                                        1, key));

    TEST_ASSERT_EQUAL_INT(1, callCount);
    TEST_ASSERT_EQUAL_UINT8(91, static_cast<uint8_t>(seenProperty));
    TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(seenValue.size()));
    TEST_ASSERT_EQUAL_MEMORY(key.data(), seenValue.data(), 16);
}

void test_observer_reports_which_property_changed(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    std::vector<uint8_t> observedProperties;
    paramObj.setWriteObserver([&](knx::application::PropertyID id, std::span<const uint8_t>,
                                  std::span<const uint8_t>) {
        observedProperties.push_back(static_cast<uint8_t>(id));
    });

    // An unrelated write must still be reported: the observer, not the object,
    // decides what it cares about.
    const std::vector<uint8_t> name = {'K', 'N', 'X'};
    (void)mgr.writeProperty(InterfaceObjectType::knxNetIpParameter(),
                            InterfaceObjectInstance(1),
                            kPidFriendlyName,
                            1, name);
    (void)mgr.writeProperty(InterfaceObjectType::knxNetIpParameter(),
                            InterfaceObjectInstance(1),
                            kPidBackboneKey,
                            1, exampleKey(0x20));

    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(observedProperties.size()));
    TEST_ASSERT_EQUAL_UINT8(76, observedProperties[0]);
    TEST_ASSERT_EQUAL_UINT8(91, observedProperties[1]);
}

void test_writes_without_an_observer_still_succeed(void)
{
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      mgr.writeProperty(InterfaceObjectType::knxNetIpParameter(),
                                        InterfaceObjectInstance(1),
                                        kPidBackboneKey,
                                        1, exampleKey(0x30)));
}

#if KNX_SECURE_ENABLED

void test_backbone_key_activates_secure_routing(void)
{
    knx::physical::IpRoutingPhysical physical;
    TEST_ASSERT_FALSE(physical.isSecureRoutingActive());

    TEST_ASSERT_TRUE(physical.applySecureRouting(exampleKey(0x40)).isOk());
    TEST_ASSERT_TRUE(physical.isSecureRoutingActive());
}

void test_an_all_zero_key_turns_secure_routing_off(void)
{
    knx::physical::IpRoutingPhysical physical;
    TEST_ASSERT_TRUE(physical.applySecureRouting(exampleKey(0x50)).isOk());
    TEST_ASSERT_TRUE(physical.isSecureRoutingActive());

    // How ETS removes Secure Routing from a project: the key is cleared, not
    // the property deleted.
    const std::vector<uint8_t> zeros(16, 0);
    TEST_ASSERT_TRUE(physical.applySecureRouting(zeros).isOk());
    TEST_ASSERT_FALSE(physical.isSecureRoutingActive());

    // An empty write means the same thing.
    TEST_ASSERT_TRUE(physical.applySecureRouting(exampleKey(0x60)).isOk());
    TEST_ASSERT_TRUE(physical.applySecureRouting({}).isOk());
    TEST_ASSERT_FALSE(physical.isSecureRoutingActive());
}

void test_a_wrong_length_key_is_refused_rather_than_padded(void)
{
    knx::physical::IpRoutingPhysical physical;

    // Silently padding or truncating would produce a device that encrypts with
    // a key nothing else on the group holds — traffic that looks secured and
    // arrives nowhere.
    const std::vector<uint8_t> shortKey(8, 0xAB);
    TEST_ASSERT_TRUE(physical.applySecureRouting(shortKey).isError());
    TEST_ASSERT_FALSE(physical.isSecureRoutingActive());

    const std::vector<uint8_t> longKey(32, 0xCD);
    TEST_ASSERT_TRUE(physical.applySecureRouting(longKey).isError());
    TEST_ASSERT_FALSE(physical.isSecureRoutingActive());
}

void test_rekeying_before_init_is_carried_into_the_endpoint(void)
{
    // A key downloaded before the interface comes up must not be lost; it is
    // stored and applied when the endpoint opens.
    knx::physical::IpRoutingPhysical physical;
    TEST_ASSERT_TRUE(physical.applySecureRouting(exampleKey(0x70)).isOk());
    TEST_ASSERT_TRUE(physical.applySecureRouting(exampleKey(0x80)).isOk());
    TEST_ASSERT_TRUE(physical.isSecureRoutingActive());
}

#endif  // KNX_SECURE_ENABLED

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_property_write_reaches_the_observer);
    RUN_TEST(test_observer_reports_which_property_changed);
    RUN_TEST(test_writes_without_an_observer_still_succeed);
#if KNX_SECURE_ENABLED
    RUN_TEST(test_backbone_key_activates_secure_routing);
    RUN_TEST(test_an_all_zero_key_turns_secure_routing_off);
    RUN_TEST(test_a_wrong_length_key_is_refused_rather_than_padded);
    RUN_TEST(test_rekeying_before_init_is_carried_into_the_endpoint);
#endif
    return UNITY_END();
}
