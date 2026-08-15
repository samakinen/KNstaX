// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_two_port_coupler.cpp
 * @brief End-to-end coupler behaviour across two data link layers.
 *
 * test_coupler_routing.cpp pins the decision; this pins what the coupler does
 * with it — which port the frame leaves by, what hop count it carries, that
 * echoes do not loop, and that a frame addressed to the coupler reaches the
 * local delivery path instead of being forwarded.
 *
 * Both ports are mock physical layers, so the test is deterministic and needs
 * no bus, no network and no timing tolerance.
 */

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/testing/mock_tp1_physical.hpp"

using namespace knx;
using namespace knx::datalink;
using namespace knx::network;

namespace {

constexpr uint16_t ia(uint8_t area, uint8_t line, uint8_t device)
{
    return static_cast<uint16_t>((area << 12) | (line << 8) | device);
}

LDataFrame makeFrame(const IndividualAddress& source, uint16_t destination, AddressType type,
                     uint8_t hopCount)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = source;
    frame.destination = GroupAddress(destination);
    frame.destinationType = type;
    frame.hopCount = hopCount;
    frame.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::GroupValueRead, {});
    return frame;
}

/**
 * @brief A coupler with both ports on mock physical layers.
 *
 * Held together in one struct because the data link layers must outlive the
 * coupler and the physical layers must outlive the data link layers.
 */
struct CouplerHarness {
    /**
     * RX processing runs inline on the injecting thread rather than on the data
     * link layer's background task. Without this the test would have to sleep
     * and hope, which turns a routing bug into an intermittent failure.
     */
    static Tp1DataLinkConfig synchronousConfig()
    {
        Tp1DataLinkConfig config;
        config.enableRxTask = false;
        return config;
    }

    platform::LinuxPlatform platform;
    testing::MockTp1Physical primaryPhysical;
    testing::MockTp1Physical secondaryPhysical;
    Tp1DataLinkLayer primaryLink;
    Tp1DataLinkLayer secondaryLink;
    TwoPortCoupler coupler;

    std::vector<std::pair<CouplerPort, LDataFrame>> locallyDelivered;
    std::vector<CouplerPort> filtered;
    std::vector<CouplerPort> dropped;

    /// @param ownAddress the coupler's individual address, which also fixes its role.
    explicit CouplerHarness(uint16_t ownAddress = ia(1, 1, 0))
        : primaryLink(platform, primaryPhysical, synchronousConfig())
        , secondaryLink(platform, secondaryPhysical, synchronousConfig())
        , coupler(primaryLink, secondaryLink)
    {
        // The data link layers are initialised with the coupler's address on
        // both sides; setOwnAddress() re-applies it and sets the routing role.
        TEST_ASSERT_TRUE(primaryLink.init(IndividualAddress(ownAddress)).isOk());
        TEST_ASSERT_TRUE(secondaryLink.init(IndividualAddress(ownAddress)).isOk());

        TEST_ASSERT_TRUE(coupler.setOwnAddress(IndividualAddress(ownAddress)).isOk());
        coupler.setRoutingEnabled(Toggle::Enable);

        coupler.setLocalDeliveryCallback([this](CouplerPort origin, const LDataFrame& frame) {
            locallyDelivered.emplace_back(origin, frame);
        });
        coupler.setFrameFilteredCallback([this](CouplerPort origin) { filtered.push_back(origin); });
        coupler.setFrameDroppedCallback([this](CouplerPort origin) { dropped.push_back(origin); });

        TEST_ASSERT_TRUE(coupler.init().isOk());
    }

    testing::MockTp1Physical& physicalFor(CouplerPort port)
    {
        return port == CouplerPort::Primary ? primaryPhysical : secondaryPhysical;
    }

    /// Push a frame onto @p port as if it had arrived off the bus.
    void inject(CouplerPort port, const LDataFrame& frame)
    {
        std::array<uint8_t, 64> raw{};
        const auto encoded = FrameCodec::encodeFrame(frame, raw);
        TEST_ASSERT_TRUE(encoded.isOk());
        physicalFor(port).injectRxFrame(std::span<const uint8_t>(raw.data(), encoded.value()));
    }

    /// The next frame transmitted on @p port, if any.
    std::optional<LDataFrame> sentOn(CouplerPort port)
    {
        std::vector<uint8_t> raw;
        if (!physicalFor(port).tryGetSentFrame(raw)) {
            return std::nullopt;
        }
        LDataFrame frame;
        const auto decoded = FrameCodec::decodeFrame(raw, frame);
        TEST_ASSERT_TRUE(decoded.isOk());
        return frame;
    }

    size_t sentCount(CouplerPort port) { return physicalFor(port).sentFrameCount(); }
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_a_group_frame_crosses_to_the_other_port_with_the_hop_count_decremented(void)
{
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0801, forwarded->destination.raw);
    TEST_ASSERT_EQUAL(5, forwarded->hopCount);

    // ...and nothing went back where it came from.
    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Primary));
}

void test_forwarding_works_in_both_directions(void)
{
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Secondary, makeFrame(IndividualAddress(ia(1, 1, 9)), 0x0801,
                                               AddressType::Group, 4));

    const auto forwarded = h.sentOn(CouplerPort::Primary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL(3, forwarded->hopCount);
    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));
}

void test_hop_count_seven_crosses_without_being_decremented(void)
{
    // The forwarded frame must still carry 7, otherwise "unlimited" would be
    // silently converted into "six more couplers".
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 7));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL(7, forwarded->hopCount);
}

void test_hop_count_one_is_still_forwarded(void)
{
    // Regression: an implementation that decrements first and then drops on
    // zero loses every frame with one hop left.
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 1));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL(0, forwarded->hopCount);
    TEST_ASSERT_EQUAL(0u, h.dropped.size());
}

void test_an_exhausted_frame_is_dropped_and_reported(void)
{
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 0));

    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));
    TEST_ASSERT_EQUAL(1u, h.dropped.size());
    // Reported as dropped, not filtered: the frame was routable and only its
    // hop count stopped it, which is a topology problem worth distinguishing.
    TEST_ASSERT_EQUAL(0u, h.filtered.size());
}

void test_the_filter_table_blocks_group_frames(void)
{
    CouplerHarness h;
    h.coupler.filterTable().setDefaultAction(FilterAction::Block);
    TEST_ASSERT_TRUE(h.coupler.filterTable()
                         .addEntry(GroupAddress(0x0801), GroupAddress(0xFFFF), FilterAction::Allow,
                                   EntryState::Enabled)
                         .isOk());
    h.coupler.setFilteringEnabled(Toggle::Enable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));
    TEST_ASSERT_EQUAL(1, h.sentCount(CouplerPort::Secondary));

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0900,
                                             AddressType::Group, 6));
    TEST_ASSERT_EQUAL(1, h.sentCount(CouplerPort::Secondary));
    TEST_ASSERT_EQUAL(1u, h.filtered.size());
}

void test_individually_addressed_frames_are_routed(void)
{
    // The capability this work added: before it, every individually addressed
    // frame was discarded, so ETS could not commission anything behind the
    // coupler.
    CouplerHarness h;  // line coupler at 1.1.0

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), ia(1, 1, 7),
                                             AddressType::Individual, 6));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL_HEX16(ia(1, 1, 7), forwarded->destination.raw);
    TEST_ASSERT_EQUAL(5, forwarded->hopCount);
}

void test_individually_addressed_frames_for_another_line_are_not_routed(void)
{
    CouplerHarness h;

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), ia(1, 3, 7),
                                             AddressType::Individual, 6));

    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));
    TEST_ASSERT_EQUAL(1u, h.filtered.size());
}

void test_a_frame_addressed_to_the_coupler_is_delivered_locally(void)
{
    CouplerHarness h;

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), ia(1, 1, 0),
                                             AddressType::Individual, 6));

    TEST_ASSERT_EQUAL(1u, h.locallyDelivered.size());
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerPort::Primary),
                      static_cast<int>(h.locallyDelivered[0].first));
    // Local delivery is not forwarding: the frame must not also appear on the
    // far side.
    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));
}

void test_the_coupler_stays_reachable_when_routing_is_disabled(void)
{
    // Turning routing off must not make the device unmanageable — otherwise
    // there would be no way to turn it back on.
    CouplerHarness h;
    h.coupler.setRoutingEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), ia(1, 1, 0),
                                             AddressType::Individual, 6));
    TEST_ASSERT_EQUAL(1u, h.locallyDelivered.size());

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));
    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));
}

void test_broadcast_crosses_the_coupler(void)
{
    // ETS discovers unaddressed devices by broadcast, so a coupler that eats
    // broadcast hides everything behind it.
    CouplerHarness h;
    h.coupler.filterTable().setDefaultAction(FilterAction::Block);
    h.coupler.setFilteringEnabled(Toggle::Enable);

    h.inject(CouplerPort::Primary,
             makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0000, AddressType::Group, 6));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());
    TEST_ASSERT_EQUAL_HEX16(0x0000, forwarded->destination.raw);
    TEST_ASSERT_EQUAL(5, forwarded->hopCount);
}

void test_a_forwarded_frame_echoed_back_does_not_loop(void)
{
    // Both ports are promiscuous, so a coupler on a shared medium sees its own
    // transmission. Without echo suppression the two ports would volley the
    // frame until its hop count ran out.
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));

    const auto forwarded = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(forwarded.has_value());

    // Feed the coupler's own transmission back in on the port it left by.
    h.inject(CouplerPort::Secondary, *forwarded);

    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Primary));
}

void test_a_backbone_coupler_routes_by_area(void)
{
    CouplerHarness h(ia(2, 0, 0));
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::BackboneCoupler),
                      static_cast<int>(h.coupler.role()));

    // Down into the area: any line of area 2.
    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(5, 1, 1)), ia(2, 4, 9),
                                             AddressType::Individual, 6));
    const auto down = h.sentOn(CouplerPort::Secondary);
    TEST_ASSERT_TRUE(down.has_value());
    TEST_ASSERT_EQUAL_HEX16(ia(2, 4, 9), down->destination.raw);

    // Up out of the area.
    h.inject(CouplerPort::Secondary, makeFrame(IndividualAddress(ia(2, 4, 9)), ia(5, 1, 1),
                                               AddressType::Individual, 6));
    const auto up = h.sentOn(CouplerPort::Primary);
    TEST_ASSERT_TRUE(up.has_value());
    TEST_ASSERT_EQUAL_HEX16(ia(5, 1, 1), up->destination.raw);
}

void test_the_coupler_configuration_reaches_the_forwarding_path(void)
{
    // A LOCK written into the policy must actually stop traffic; the property
    // being settable is not the same as it being obeyed.
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);
    h.coupler.policy().groupConfig(CouplerPort::Primary).group6FFF = GroupHandling::Lock;

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));
    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));

    // The other direction is governed by the other property and is untouched.
    h.inject(CouplerPort::Secondary, makeFrame(IndividualAddress(ia(1, 1, 9)), 0x0801,
                                               AddressType::Group, 6));
    TEST_ASSERT_EQUAL(1, h.sentCount(CouplerPort::Primary));
}

void test_group_frames_are_delivered_locally_as_well_as_routed(void)
{
    // A coupler can carry group objects of its own. Routing and local reception
    // are independent concerns: the coupler decides what crosses, the device's
    // own address table decides what it listens to. Making them exclusive would
    // mean a coupler could never subscribe to a group address it also forwards.
    CouplerHarness h;
    h.coupler.setFilteringEnabled(Toggle::Disable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0801,
                                             AddressType::Group, 6));

    TEST_ASSERT_EQUAL(1, h.sentCount(CouplerPort::Secondary));   // routed
    TEST_ASSERT_EQUAL(1u, h.locallyDelivered.size());            // and offered locally
}

void test_a_filtered_group_frame_is_still_offered_locally(void)
{
    // The case that makes the independence matter. The filter table stops the
    // telegram crossing to the other line, but this device may still be one of
    // its intended recipients.
    CouplerHarness h;
    h.coupler.filterTable().setDefaultAction(FilterAction::Block);
    h.coupler.setFilteringEnabled(Toggle::Enable);

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), 0x0900,
                                             AddressType::Group, 6));

    TEST_ASSERT_EQUAL(0, h.sentCount(CouplerPort::Secondary));   // not routed
    TEST_ASSERT_EQUAL(1u, h.locallyDelivered.size());            // still ours to judge
}

void test_transit_individual_frames_are_not_delivered_locally(void)
{
    // The counterpart: an individually addressed frame passing through is not
    // this device's business. Delivering it would hand the application layer
    // management traffic aimed at some other device.
    CouplerHarness h;

    h.inject(CouplerPort::Primary, makeFrame(IndividualAddress(ia(1, 2, 5)), ia(1, 1, 7),
                                             AddressType::Individual, 6));

    TEST_ASSERT_EQUAL(1, h.sentCount(CouplerPort::Secondary));   // routed onward
    TEST_ASSERT_EQUAL(0u, h.locallyDelivered.size());            // but not ours
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_a_group_frame_crosses_to_the_other_port_with_the_hop_count_decremented);
    RUN_TEST(test_forwarding_works_in_both_directions);
    RUN_TEST(test_hop_count_seven_crosses_without_being_decremented);
    RUN_TEST(test_hop_count_one_is_still_forwarded);
    RUN_TEST(test_an_exhausted_frame_is_dropped_and_reported);
    RUN_TEST(test_the_filter_table_blocks_group_frames);
    RUN_TEST(test_individually_addressed_frames_are_routed);
    RUN_TEST(test_individually_addressed_frames_for_another_line_are_not_routed);
    RUN_TEST(test_a_frame_addressed_to_the_coupler_is_delivered_locally);
    RUN_TEST(test_the_coupler_stays_reachable_when_routing_is_disabled);
    RUN_TEST(test_broadcast_crosses_the_coupler);
    RUN_TEST(test_a_forwarded_frame_echoed_back_does_not_loop);
    RUN_TEST(test_a_backbone_coupler_routes_by_area);
    RUN_TEST(test_the_coupler_configuration_reaches_the_forwarding_path);
    RUN_TEST(test_group_frames_are_delivered_locally_as_well_as_routed);
    RUN_TEST(test_a_filtered_group_frame_is_still_offered_locally);
    RUN_TEST(test_transit_individual_frames_are_not_delivered_locally);

    return UNITY_END();
}
