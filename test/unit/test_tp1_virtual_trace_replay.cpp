// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#define private public
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#undef private

#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"
#include "knx/physical/virtual_tp1_trace.hpp"

using namespace knx::physical;

namespace {

BitBangDriverTimerIsr driver;
TimerGpioHalVirtualBus virtualBus;
VirtualTp1BusPeer busPeer(virtualBus);
knx_timer_gpio_hal_t hal{};
BitBangConfig config{};

bool popDataByte(BitBangDriverTimerIsr& localDriver, uint8_t expected)
{
    BitBangDriverTimerIsr::Message message{};
    while (localDriver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Data && message.data == expected) {
            return true;
        }
    }
    return false;
}

std::vector<VirtualTp1Trace::Event> buildGpioEventsFromInjected(
    const std::vector<VirtualTp1BusPeer::RxEdge>& edges,
    VirtualTp1Trace::EventSource source)
{
    std::vector<VirtualTp1Trace::Event> events;
    events.reserve(edges.size());
    for (const auto& edge : edges) {
        VirtualTp1Trace::Event event;
        event.tsUs = edge.timestampUs;
        event.type = VirtualTp1Trace::EventType::GpioEdge;
        event.source = source;
        event.pin = 5;
        event.level = edge.level;
        event.meta = 0;
        events.push_back(event);
    }
    return events;
}

void setupDefaultConfig()
{
    config = BitBangConfig{};
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
}

} // namespace

void setUp()
{
    virtualBus.reset();
    busPeer.clearScript();
    (void)virtualBus.bind(hal);
    setupDefaultConfig();
    TEST_ASSERT_TRUE(driver.init(hal, config));
}

void tearDown()
{
    driver.shutdown();
}

void test_VTRACE_001_record_nominal_trace()
{
    const uint8_t frame[1] = {0x00};
    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(16000);

    const auto events = VirtualTp1Trace::captureTxTransitions(virtualBus.capturedTxTransitions());
    TEST_ASSERT_FALSE(events.empty());

    VirtualTp1Trace::Header header{};
    header.traceVersionMajor = 1;
    header.traceVersionMinor = 0;
    header.simProfile = "tp1-virtual-mvp";
    header.seed = 7;

    const std::string payload = VirtualTp1Trace::encode(header, events);
    TEST_ASSERT_TRUE(payload.find("TRACE|1|0|tp1-virtual-mvp|7") == 0);

    VirtualTp1Trace::Header decodedHeader{};
    std::vector<VirtualTp1Trace::Event> decodedEvents;
    TEST_ASSERT_TRUE(VirtualTp1Trace::decode(payload, decodedHeader, decodedEvents));
    TEST_ASSERT_EQUAL_UINT32(1u, decodedHeader.traceVersionMajor);
    TEST_ASSERT_EQUAL_STRING("tp1-virtual-mvp", decodedHeader.simProfile.c_str());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(events.size()), static_cast<uint32_t>(decodedEvents.size()));
}

void test_VTRACE_002_replay_is_deterministic()
{
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(20u, 0x5A, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());

    const auto sourceEvents = buildGpioEventsFromInjected(busPeer.lastInjectedEdges(),
                                                          VirtualTp1Trace::EventSource::BusPeer);

    VirtualTp1Trace::Header header{};
    header.traceVersionMajor = 1;
    header.traceVersionMinor = 0;
    header.simProfile = "tp1-virtual-mvp";
    header.seed = 0;

    const std::string payloadA = VirtualTp1Trace::encode(header, sourceEvents);

    VirtualTp1Trace::Header decodedHeader{};
    std::vector<VirtualTp1Trace::Event> decodedEvents;
    TEST_ASSERT_TRUE(VirtualTp1Trace::decode(payloadA, decodedHeader, decodedEvents));

    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);
    TEST_ASSERT_TRUE(driver.init(hal, config));

    TEST_ASSERT_TRUE(VirtualTp1Trace::replayToVirtualBus(virtualBus, decodedEvents));
    virtualBus.advanceTimeUs(2600);
    TEST_ASSERT_TRUE(popDataByte(driver, 0x5A));

    const std::string payloadB = VirtualTp1Trace::encode(decodedHeader, decodedEvents);
    TEST_ASSERT_EQUAL_STRING(payloadA.c_str(), payloadB.c_str());
}

void test_VTRACE_003_seeded_random_scenario_replay()
{
    VirtualTp1BusPeer::FaultProfile faults{};
    faults.jitterUs = 2;
    faults.dropEveryN = 0;
    faults.duplicateEveryN = 3;
    faults.duplicateSpacingUs = 1;
    faults.seed = 0xACE1u;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(40u, 0x33, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));

    const auto firstEvents = buildGpioEventsFromInjected(busPeer.lastInjectedEdges(),
                                                         VirtualTp1Trace::EventSource::FaultInjector);

    driver.shutdown();
    virtualBus.reset();
    busPeer.clearScript();
    (void)virtualBus.bind(hal);
    TEST_ASSERT_TRUE(driver.init(hal, config));

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(40u, 0x33, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));
    const auto secondEvents = buildGpioEventsFromInjected(busPeer.lastInjectedEdges(),
                                                          VirtualTp1Trace::EventSource::FaultInjector);

    VirtualTp1Trace::Header header{};
    header.traceVersionMajor = 1;
    header.traceVersionMinor = 0;
    header.simProfile = "tp1-virtual-mvp";
    header.seed = faults.seed;

    const std::string firstPayload = VirtualTp1Trace::encode(header, firstEvents);
    const std::string secondPayload = VirtualTp1Trace::encode(header, secondEvents);
    TEST_ASSERT_EQUAL_STRING(firstPayload.c_str(), secondPayload.c_str());

    VirtualTp1Trace::Header decodedHeader{};
    std::vector<VirtualTp1Trace::Event> decodedEvents;
    TEST_ASSERT_TRUE(VirtualTp1Trace::decode(firstPayload, decodedHeader, decodedEvents));

    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);
    TEST_ASSERT_TRUE(driver.init(hal, config));

    TEST_ASSERT_TRUE(VirtualTp1Trace::replayToVirtualBus(virtualBus, decodedEvents));
    virtualBus.advanceTimeUs(3000);
    TEST_ASSERT_TRUE(popDataByte(driver, 0x33));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTRACE_001_record_nominal_trace);
    RUN_TEST(test_VTRACE_002_replay_is_deterministic);
    RUN_TEST(test_VTRACE_003_seeded_random_scenario_replay);
    return UNITY_END();
}
