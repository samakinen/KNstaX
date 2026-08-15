// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_coupler_routing.cpp
 * @brief The coupler routing algorithm against the specification tables.
 *
 * 03/03/03 §2.4.2.4 states the routing algorithm as a set of decision tables,
 * one per coupler role and direction. These tests walk those tables row by row.
 * Where a test looks redundant it is usually pinning the difference between two
 * outcomes that a naive implementation collapses — IGNORE_TOTALLY versus
 * IGNORE_ACKED, or ROUTE_UNMODIFIED versus ROUTE_DECREMENTED — and those
 * distinctions are the entire reason the specification enumerates six actions
 * rather than two.
 *
 * The configuration byte tests come from 03/05/01 §4.4.4 and §4.4.5.
 */

#include "unity.h"

#include "knx/network/coupler_routing.hpp"
#include "knx/network/filter_table.hpp"

using namespace knx;
using namespace knx::network;

namespace {

/// A frame carrying only what the routing algorithm looks at.
datalink::LDataFrame makeFrame(uint16_t destination, AddressType type, uint8_t hopCount)
{
    datalink::LDataFrame frame{};
    frame.standardFrame = true;
    frame.priority = Priority::Low;
    frame.source = IndividualAddress(1, 1, 5);
    frame.destination = GroupAddress(destination);
    frame.destinationType = type;
    frame.hopCount = hopCount;
    return frame;
}

datalink::LDataFrame individualFrame(uint16_t destination, uint8_t hopCount = 6)
{
    return makeFrame(destination, AddressType::Individual, hopCount);
}

datalink::LDataFrame groupFrame(uint16_t destination, uint8_t hopCount = 6)
{
    return makeFrame(destination, AddressType::Group, hopCount);
}

datalink::LDataFrame broadcastFrame(uint8_t hopCount = 6)
{
    return makeFrame(0x0000, AddressType::Group, hopCount);
}

/// Individual address from the familiar area.line.device notation.
constexpr uint16_t ia(uint8_t area, uint8_t line, uint8_t device)
{
    return static_cast<uint16_t>((area << 12) | (line << 8) | device);
}

const char* actionName(RoutingAction action)
{
    switch (action) {
        case RoutingAction::RouteUnmodified:  return "ROUTE_UNMODIFIED";
        case RoutingAction::RouteDecremented: return "ROUTE_DECREMENTED";
        case RoutingAction::RouteLast:        return "ROUTE_LAST";
        case RoutingAction::ForwardLocally:   return "FORWARD_LOCALLY";
        case RoutingAction::IgnoreTotally:    return "IGNORE_TOTALLY";
        case RoutingAction::IgnoreAcked:      return "IGNORE_ACKED";
    }
    return "?";
}

void assertAction(RoutingAction expected, RoutingAction actual, const char* what)
{
    if (expected != actual) {
        char message[192];
        std::snprintf(message, sizeof(message), "%s: expected %s, got %s", what,
                      actionName(expected), actionName(actual));
        TEST_FAIL_MESSAGE(message);
    }
}

/// A line coupler for line 1.1, i.e. individual address 1.1.0.
CouplerRoutingPolicy lineCoupler()
{
    CouplerRoutingPolicy policy;
    policy.setOwnAddress(IndividualAddress(ia(1, 1, 0)));
    return policy;
}

/// A backbone coupler for area 1, i.e. individual address 1.0.0.
CouplerRoutingPolicy backboneCoupler()
{
    CouplerRoutingPolicy policy;
    policy.setOwnAddress(IndividualAddress(ia(1, 0, 0)));
    return policy;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Role derivation (03/03/03 §2.4.2.4)
// -----------------------------------------------------------------------------

void test_role_is_derived_from_the_individual_address(void)
{
    // A backbone coupler sits at x.0.0, a line coupler at x.y.0 with y != 0.
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::BackboneCoupler),
                      static_cast<int>(CouplerRoutingPolicy::roleFor(IndividualAddress(ia(1, 0, 0)))));
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::LineCoupler),
                      static_cast<int>(CouplerRoutingPolicy::roleFor(IndividualAddress(ia(1, 1, 0)))));
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::LineCoupler),
                      static_cast<int>(CouplerRoutingPolicy::roleFor(IndividualAddress(ia(15, 15, 0)))));
}

void test_a_device_address_is_not_a_coupler_address(void)
{
    // Device part != 0 means an ordinary device. Guessing a coupler role for it
    // would route traffic against a subnetwork the device does not own.
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::Repeater),
                      static_cast<int>(CouplerRoutingPolicy::roleFor(IndividualAddress(ia(1, 1, 5)))));
    // Area 0 is not a valid coupler area either.
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::Repeater),
                      static_cast<int>(CouplerRoutingPolicy::roleFor(IndividualAddress(ia(0, 0, 0)))));
}

// -----------------------------------------------------------------------------
// Hop count handling, shared by every rule
// -----------------------------------------------------------------------------

void test_hop_count_seven_is_routed_unmodified(void)
{
    // 7 is the "unlimited" marker, not a count. Decrementing it would age a
    // frame that is explicitly meant to cross any number of couplers — the bug
    // that makes long-haul management traffic die three couplers in.
    auto policy = lineCoupler();
    auto frame = groupFrame(0x0801, 7);
    assertAction(RoutingAction::RouteUnmodified,
                 policy.decide(CouplerPort::Primary, frame), "hop count 7");
    TEST_ASSERT_EQUAL(7, CouplerRoutingPolicy::applyHopCount(RoutingAction::RouteUnmodified, 7));
}

void test_hop_counts_one_through_six_are_decremented(void)
{
    auto policy = lineCoupler();
    for (uint8_t hop = 1; hop <= 6; ++hop) {
        auto frame = groupFrame(0x0801, hop);
        assertAction(RoutingAction::RouteDecremented,
                     policy.decide(CouplerPort::Primary, frame), "hop counts 1..6");
        TEST_ASSERT_EQUAL(hop - 1,
                          CouplerRoutingPolicy::applyHopCount(RoutingAction::RouteDecremented, hop));
    }
}

void test_hop_count_one_is_forwarded_with_zero_not_dropped(void)
{
    // The off-by-one worth pinning: hop count 1 has one hop left, so it must be
    // forwarded carrying 0, not discarded. Dropping it silently shortens every
    // route in the installation by one coupler.
    auto policy = lineCoupler();
    auto frame = groupFrame(0x0801, 1);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "hop count 1");
    TEST_ASSERT_EQUAL(0, CouplerRoutingPolicy::applyHopCount(RoutingAction::RouteDecremented, 1));
}

void test_exhausted_hop_count_is_acknowledged_not_ignored(void)
{
    // IGNORE_ACKED, not IGNORE_TOTALLY: the frame was meant to be routed, so
    // acknowledging it stops the sender retransmitting something that can never
    // make progress.
    auto policy = lineCoupler();
    auto frame = groupFrame(0x0801, 0);
    assertAction(RoutingAction::IgnoreAcked,
                 policy.decide(CouplerPort::Primary, frame), "hop count 0");
}

// -----------------------------------------------------------------------------
// Line coupler, point-to-point (03/03/03 §2.4.2.4.2)
// -----------------------------------------------------------------------------

void test_line_coupler_main_to_sub_routes_traffic_for_its_own_line(void)
{
    // §2.4.2.4.2.1: ZS matches and D != 0 → route down to the sub line.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 1, 5));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "main→sub, own line");
}

void test_line_coupler_main_to_sub_ignores_traffic_for_another_line(void)
{
    // IGNORE_TOTALLY, not IGNORE_ACKED: this frame is somebody else's, so
    // acknowledging it would tell the sender it had been delivered.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 2, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "main→sub, other line");

    auto otherArea = individualFrame(ia(2, 1, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, otherArea), "main→sub, other area");
}

void test_line_coupler_main_to_sub_delivers_its_own_address_locally(void)
{
    // D == 0 on the coupler's own subnetwork is the coupler itself. This is how
    // ETS reaches the coupler to commission it, so losing it makes the device
    // unmanageable.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 1, 0));
    assertAction(RoutingAction::ForwardLocally,
                 policy.decide(CouplerPort::Primary, frame), "main→sub, own address");
}

void test_line_coupler_sub_to_main_routes_traffic_leaving_the_line(void)
{
    // §2.4.2.4.2.2: ZS differs → the destination is off this line, so route up.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 2, 5));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Secondary, frame), "sub→main, other line");

    auto otherArea = individualFrame(ia(3, 4, 5));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Secondary, otherArea), "sub→main, other area");
}

void test_line_coupler_sub_to_main_ignores_traffic_staying_on_the_line(void)
{
    // Both endpoints are on the sub line, so the frame has already arrived.
    // Forwarding it would echo local traffic onto the main line.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 1, 7));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Secondary, frame), "sub→main, same line");
}

void test_line_coupler_sub_to_main_delivers_its_own_address_locally(void)
{
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(1, 1, 0));
    assertAction(RoutingAction::ForwardLocally,
                 policy.decide(CouplerPort::Secondary, frame), "sub→main, own address");
}

// -----------------------------------------------------------------------------
// Backbone coupler, point-to-point (03/03/03 §2.4.2.4.3)
// -----------------------------------------------------------------------------

void test_backbone_coupler_matches_on_area_not_subnetwork(void)
{
    // §2.4.2.4.3.1: a backbone coupler compares Z (the area nibble) only, so
    // every line in its area is routed down, not just its own main line. A
    // backbone coupler that reuses the line-coupler rule strands whole lines.
    auto policy = backboneCoupler();
    for (uint8_t line = 1; line <= 3; ++line) {
        auto frame = individualFrame(ia(1, line, 5));
        assertAction(RoutingAction::RouteDecremented,
                     policy.decide(CouplerPort::Primary, frame), "backbone→main, own area");
    }
}

void test_backbone_coupler_backbone_to_main_ignores_other_areas(void)
{
    auto policy = backboneCoupler();
    auto frame = individualFrame(ia(2, 1, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "backbone→main, other area");
}

void test_backbone_coupler_backbone_to_main_delivers_its_own_address_locally(void)
{
    // SD == 0 means line and device parts are both zero: the coupler itself.
    auto policy = backboneCoupler();
    auto frame = individualFrame(ia(1, 0, 0));
    assertAction(RoutingAction::ForwardLocally,
                 policy.decide(CouplerPort::Primary, frame), "backbone→main, own address");
}

void test_backbone_coupler_main_to_backbone_routes_out_of_area(void)
{
    // §2.4.2.4.3.2.
    auto policy = backboneCoupler();
    auto frame = individualFrame(ia(2, 3, 4));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Secondary, frame), "main→backbone, other area");
}

void test_backbone_coupler_main_to_backbone_ignores_traffic_within_the_area(void)
{
    auto policy = backboneCoupler();
    auto frame = individualFrame(ia(1, 2, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Secondary, frame), "main→backbone, own area");
}

void test_backbone_coupler_main_to_backbone_delivers_its_own_address_locally(void)
{
    auto policy = backboneCoupler();
    auto frame = individualFrame(ia(1, 0, 0));
    assertAction(RoutingAction::ForwardLocally,
                 policy.decide(CouplerPort::Secondary, frame), "main→backbone, own address");
}

// -----------------------------------------------------------------------------
// PHYS_FRAME overrides (03/05/01 §4.4.4)
// -----------------------------------------------------------------------------

void test_phys_lock_blocks_point_to_point_routing(void)
{
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physFrame = PhysFrameHandling::Lock;

    auto frame = individualFrame(ia(1, 1, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "PHYS_LOCK");
}

void test_phys_lock_still_delivers_frames_addressed_to_the_coupler(void)
{
    // PHYS_LOCK stops the coupler *routing*; it does not stop it being a
    // device. If it did, locking a coupler would make it unreachable by ETS and
    // therefore impossible to unlock again.
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physFrame = PhysFrameHandling::Lock;

    auto frame = individualFrame(ia(1, 1, 0));
    assertAction(RoutingAction::ForwardLocally,
                 policy.decide(CouplerPort::Primary, frame), "PHYS_LOCK, own address");
}

void test_phys_unlock_routes_regardless_of_address(void)
{
    // Bridge mode: forward point-to-point traffic that the normal algorithm
    // would have ignored as belonging to another line.
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physFrame = PhysFrameHandling::Unlock;

    auto frame = individualFrame(ia(7, 7, 7));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "PHYS_UNLOCK");
}

void test_phys_frame_config_is_per_direction(void)
{
    // MAIN governs primary→secondary and SUB governs secondary→primary, so
    // locking one direction must leave the other working.
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physFrame = PhysFrameHandling::Lock;

    auto down = individualFrame(ia(1, 1, 5));
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, down), "primary locked");

    auto up = individualFrame(ia(1, 2, 5));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Secondary, up), "secondary still open");
}

// -----------------------------------------------------------------------------
// Group / multicast routing (03/03/03 §2.4.2.4.1, 03/05/01 §4.4.5)
// -----------------------------------------------------------------------------

void test_group_routing_consults_the_filter_table(void)
{
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);
    TEST_ASSERT_TRUE(table.addEntry(GroupAddress(0x0801), GroupAddress(0xFFFF),
                                    FilterAction::Allow, EntryState::Enabled)
                         .isOk());

    auto policy = lineCoupler();
    policy.setFilterTable(&table);

    auto allowed = groupFrame(0x0801);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, allowed), "filter allows");

    auto blocked = groupFrame(0x0802);
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, blocked), "filter blocks");
}

void test_a_filtered_group_frame_is_ignored_totally_even_at_hop_count_seven(void)
{
    // The reading that matters: the routing condition gates the hop-count rule
    // rather than sitting beside it. Otherwise every hop-count-7 telegram would
    // bypass the filter table and the filter table would be decorative.
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);

    auto frame = groupFrame(0x0802, 7);
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "blocked at hop count 7");
}

void test_group_lock_blocks_everything_in_its_range(void)
{
    FilterTable table;
    table.setDefaultAction(FilterAction::Allow);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);
    policy.groupConfig(CouplerPort::Primary).group6FFF = GroupHandling::Lock;

    auto frame = groupFrame(0x0801);
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "GROUP_LOCK6FFF");
}

void test_group_unlock_bypasses_the_filter_table(void)
{
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);
    policy.groupConfig(CouplerPort::Primary).group6FFF = GroupHandling::Unlock;

    auto frame = groupFrame(0x0801);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "GROUP_UNLOCK6FFF");
}

void test_the_two_group_ranges_are_configured_separately(void)
{
    // §4.4.5 splits at 0x6FFF because ETS only downloads filter entries for the
    // lower range. Applying one setting to both either filters addresses that
    // were never in the table, or stops filtering ones that were.
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);
    policy.groupConfig(CouplerPort::Primary).group6FFF = GroupHandling::Route;
    policy.groupConfig(CouplerPort::Primary).group7000 = GroupHandling::Unlock;

    auto low = groupFrame(0x6FFF);
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, low), "0x6FFF uses GROUP_6FFF");

    auto high = groupFrame(0x7000);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, high), "0x7000 uses GROUP_7000");
}

void test_group_routing_passes_everything_without_a_filter_table(void)
{
    // A coupler whose filter table has not been downloaded yet must not sever
    // the line. Failing open here is the difference between an installation
    // that works before commissioning and one that appears dead.
    auto policy = lineCoupler();
    TEST_ASSERT_NULL(policy.filterTable());

    auto frame = groupFrame(0x0801);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "no filter table");
}

void test_filter_table_use_disabled_bypasses_filtering(void)
{
    // PID_FILTER_TABLE_USE = 0 degrades GROUP_ROUT to GROUP_UNLOCK rather than
    // blocking everything.
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);
    policy.setFilterTableInUse(false);

    auto frame = groupFrame(0x0801);
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "PID_FILTER_TABLE_USE = 0");
}

// -----------------------------------------------------------------------------
// Broadcast (03/03/03 §2.4.2.4.4)
// -----------------------------------------------------------------------------

void test_broadcast_is_routed_by_default(void)
{
    // Broadcast is how ETS finds unaddressed devices. A coupler that blocks it
    // by default makes everything behind it invisible to commissioning.
    auto policy = lineCoupler();
    auto frame = broadcastFrame();
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "broadcast");
    TEST_ASSERT_EQUAL(static_cast<int>(FrameClass::Broadcast),
                      static_cast<int>(CouplerRoutingPolicy::classify(frame)));
}

void test_broadcast_lock_blocks_broadcast(void)
{
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).broadcastLock = true;

    auto frame = broadcastFrame();
    assertAction(RoutingAction::IgnoreTotally,
                 policy.decide(CouplerPort::Primary, frame), "BROADCAST_LOCK");
}

void test_broadcast_is_not_subject_to_the_filter_table(void)
{
    // Broadcast is group address 0x0000 but is not multicast; running it
    // through the filter table would block it in every real installation, since
    // no filter table contains 0/0/0.
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);

    auto policy = lineCoupler();
    policy.setFilterTable(&table);

    auto frame = broadcastFrame();
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "broadcast vs filter table");
}

void test_system_broadcast_is_routed_last(void)
{
    // §2.4.2.4.4.2: ROUTE_LAST, so the frame is marked as unable to travel
    // beyond the next coupler. Passed explicitly because TP1 cannot distinguish
    // a system broadcast from an ordinary one.
    auto policy = lineCoupler();
    auto frame = broadcastFrame(6);
    assertAction(RoutingAction::RouteLast,
                 policy.decide(CouplerPort::Primary, frame, FrameClass::SystemBroadcast),
                 "system broadcast");
    TEST_ASSERT_EQUAL(0, CouplerRoutingPolicy::applyHopCount(RoutingAction::RouteLast, 6));

    auto unlimited = broadcastFrame(7);
    assertAction(RoutingAction::RouteUnmodified,
                 policy.decide(CouplerPort::Primary, unlimited, FrameClass::SystemBroadcast),
                 "system broadcast, hop count 7");
}

// -----------------------------------------------------------------------------
// Repeater
// -----------------------------------------------------------------------------

void test_a_repeater_forwards_point_to_point_traffic_unconditionally(void)
{
    // With no subnetwork of its own there is no address comparison to make.
    CouplerRoutingPolicy policy;
    TEST_ASSERT_EQUAL(static_cast<int>(CouplerRole::Repeater), static_cast<int>(policy.role()));

    auto frame = individualFrame(ia(5, 5, 5));
    assertAction(RoutingAction::RouteDecremented,
                 policy.decide(CouplerPort::Primary, frame), "repeater");
}

// -----------------------------------------------------------------------------
// Layer-2 acknowledge (03/05/01 §4.4.4)
// -----------------------------------------------------------------------------

void test_routed_and_locally_delivered_frames_are_acknowledged(void)
{
    auto policy = lineCoupler();

    auto routed = individualFrame(ia(1, 1, 5));
    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::Acknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, routed,
                                                        RoutingAction::RouteDecremented)));

    auto local = individualFrame(ia(1, 1, 0));
    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::Acknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, local,
                                                        RoutingAction::ForwardLocally)));
}

void test_ignore_totally_is_not_acknowledged_but_ignore_acked_is(void)
{
    // The entire point of having two ignore actions.
    auto policy = lineCoupler();
    auto frame = individualFrame(ia(2, 2, 2));

    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::NoAcknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, frame,
                                                        RoutingAction::IgnoreTotally)));
    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::Acknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, frame,
                                                        RoutingAction::IgnoreAcked)));
}

void test_phys_iack_nack_refuses_point_to_point_traffic(void)
{
    // Protects a subnetwork from being reparameterised from the other side.
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physAck = PhysAckHandling::Nack;

    auto frame = individualFrame(ia(1, 1, 5));
    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::NegativeAcknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, frame,
                                                        RoutingAction::RouteDecremented)));
}

void test_group_iack_rout_zero_acknowledges_even_filtered_frames(void)
{
    // The "acknowledge everything" mode, which exists purely to stop misrouted
    // frames being retransmitted.
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).groupAckRoutedOnly = false;

    auto frame = groupFrame(0x0801);
    TEST_ASSERT_EQUAL(static_cast<int>(AckPolicy::Acknowledge),
                      static_cast<int>(policy.ackPolicy(CouplerPort::Primary, frame,
                                                        RoutingAction::IgnoreTotally)));
}

// -----------------------------------------------------------------------------
// Configuration byte encodings (03/05/01 §4.4.4, §4.4.5)
// -----------------------------------------------------------------------------

void test_line_coupler_config_defaults_match_the_specification(void)
{
    // The bits marked "(d)" in §4.4.4: PHYS_ROUT, PHYS_REPEAT, broadcasts
    // routed and repeated, GROUP_IACK_ROUT normal, PHYS_IACK normal.
    const LineCouplerConfig config;
    TEST_ASSERT_EQUAL_HEX8(kDefaultLineCouplerConfig, config.encode());
    TEST_ASSERT_EQUAL_HEX8(0x77, config.encode());
}

void test_group_coupler_config_defaults_match_the_specification(void)
{
    // §4.4.5: GROUP_ROUT6FFF, GROUP_UNLOCK7000, GROUP_REPEAT. Note that the
    // two ranges have *different* defaults.
    const GroupCouplerConfig config;
    TEST_ASSERT_EQUAL_HEX8(kDefaultGroupCouplerConfig, config.encode());
    TEST_ASSERT_EQUAL_HEX8(0x17, config.encode());
}

void test_line_coupler_config_round_trips(void)
{
    for (unsigned raw = 0; raw <= 0xFF; ++raw) {
        const auto config = LineCouplerConfig::decode(static_cast<uint8_t>(raw));
        const uint8_t encoded = config.encode();

        // Encoding 0 in either two-bit field means "not used" and decodes to
        // the default, so those bytes are not expected to survive a round trip.
        const bool physFrameReserved = (raw & 0x03) == 0;
        const bool physAckReserved = ((raw >> 6) & 0x03) == 0;
        if (physFrameReserved || physAckReserved) continue;

        TEST_ASSERT_EQUAL_HEX8(raw, encoded);
    }
}

void test_group_coupler_config_round_trips(void)
{
    for (unsigned raw = 0; raw <= 0xFF; ++raw) {
        // Bits 5-7 are "shall be 0" and are not carried.
        if ((raw & 0xE0) != 0) continue;
        if ((raw & 0x03) == 0 || ((raw >> 2) & 0x03) == 0) continue;

        const auto config = GroupCouplerConfig::decode(static_cast<uint8_t>(raw));
        TEST_ASSERT_EQUAL_HEX8(raw, config.encode());
    }
}

void test_config_decoding_maps_the_named_fields(void)
{
    const auto config = LineCouplerConfig::decode(0xC9);  // 1100 1001
    TEST_ASSERT_EQUAL(static_cast<int>(PhysFrameHandling::Unlock),
                      static_cast<int>(config.physFrame));       // bits 0-1 = 01
    TEST_ASSERT_FALSE(config.physRepeat);                        // bit 2 = 0
    TEST_ASSERT_TRUE(config.broadcastLock);                      // bit 3 = 1
    TEST_ASSERT_FALSE(config.broadcastRepeat);                   // bit 4 = 0
    TEST_ASSERT_FALSE(config.groupAckRoutedOnly);                // bit 5 = 0
    TEST_ASSERT_EQUAL(static_cast<int>(PhysAckHandling::Nack),
                      static_cast<int>(config.physAck));         // bits 6-7 = 11

    const auto groupConfig = GroupCouplerConfig::decode(0x16);   // 0001 0110
    TEST_ASSERT_EQUAL(static_cast<int>(GroupHandling::Lock),
                      static_cast<int>(groupConfig.group6FFF));  // bits 0-1 = 10
    TEST_ASSERT_EQUAL(static_cast<int>(GroupHandling::Unlock),
                      static_cast<int>(groupConfig.group7000));  // bits 2-3 = 01
    TEST_ASSERT_TRUE(groupConfig.groupRepeat);                   // bit 4 = 1
}

void test_repetition_follows_the_frame_class(void)
{
    auto policy = lineCoupler();
    policy.lineConfig(CouplerPort::Primary).physRepeat = false;
    policy.lineConfig(CouplerPort::Primary).broadcastRepeat = true;
    policy.groupConfig(CouplerPort::Primary).groupRepeat = false;

    auto p2p = individualFrame(ia(1, 1, 5));
    TEST_ASSERT_FALSE(policy.shouldRepeat(CouplerPort::Primary, p2p));

    auto group = groupFrame(0x0801);
    TEST_ASSERT_FALSE(policy.shouldRepeat(CouplerPort::Primary, group));

    auto broadcast = broadcastFrame();
    TEST_ASSERT_TRUE(policy.shouldRepeat(CouplerPort::Primary, broadcast));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_role_is_derived_from_the_individual_address);
    RUN_TEST(test_a_device_address_is_not_a_coupler_address);

    RUN_TEST(test_hop_count_seven_is_routed_unmodified);
    RUN_TEST(test_hop_counts_one_through_six_are_decremented);
    RUN_TEST(test_hop_count_one_is_forwarded_with_zero_not_dropped);
    RUN_TEST(test_exhausted_hop_count_is_acknowledged_not_ignored);

    RUN_TEST(test_line_coupler_main_to_sub_routes_traffic_for_its_own_line);
    RUN_TEST(test_line_coupler_main_to_sub_ignores_traffic_for_another_line);
    RUN_TEST(test_line_coupler_main_to_sub_delivers_its_own_address_locally);
    RUN_TEST(test_line_coupler_sub_to_main_routes_traffic_leaving_the_line);
    RUN_TEST(test_line_coupler_sub_to_main_ignores_traffic_staying_on_the_line);
    RUN_TEST(test_line_coupler_sub_to_main_delivers_its_own_address_locally);

    RUN_TEST(test_backbone_coupler_matches_on_area_not_subnetwork);
    RUN_TEST(test_backbone_coupler_backbone_to_main_ignores_other_areas);
    RUN_TEST(test_backbone_coupler_backbone_to_main_delivers_its_own_address_locally);
    RUN_TEST(test_backbone_coupler_main_to_backbone_routes_out_of_area);
    RUN_TEST(test_backbone_coupler_main_to_backbone_ignores_traffic_within_the_area);
    RUN_TEST(test_backbone_coupler_main_to_backbone_delivers_its_own_address_locally);

    RUN_TEST(test_phys_lock_blocks_point_to_point_routing);
    RUN_TEST(test_phys_lock_still_delivers_frames_addressed_to_the_coupler);
    RUN_TEST(test_phys_unlock_routes_regardless_of_address);
    RUN_TEST(test_phys_frame_config_is_per_direction);

    RUN_TEST(test_group_routing_consults_the_filter_table);
    RUN_TEST(test_a_filtered_group_frame_is_ignored_totally_even_at_hop_count_seven);
    RUN_TEST(test_group_lock_blocks_everything_in_its_range);
    RUN_TEST(test_group_unlock_bypasses_the_filter_table);
    RUN_TEST(test_the_two_group_ranges_are_configured_separately);
    RUN_TEST(test_group_routing_passes_everything_without_a_filter_table);
    RUN_TEST(test_filter_table_use_disabled_bypasses_filtering);

    RUN_TEST(test_broadcast_is_routed_by_default);
    RUN_TEST(test_broadcast_lock_blocks_broadcast);
    RUN_TEST(test_broadcast_is_not_subject_to_the_filter_table);
    RUN_TEST(test_system_broadcast_is_routed_last);

    RUN_TEST(test_a_repeater_forwards_point_to_point_traffic_unconditionally);

    RUN_TEST(test_routed_and_locally_delivered_frames_are_acknowledged);
    RUN_TEST(test_ignore_totally_is_not_acknowledged_but_ignore_acked_is);
    RUN_TEST(test_phys_iack_nack_refuses_point_to_point_traffic);
    RUN_TEST(test_group_iack_rout_zero_acknowledges_even_filtered_frames);

    RUN_TEST(test_line_coupler_config_defaults_match_the_specification);
    RUN_TEST(test_group_coupler_config_defaults_match_the_specification);
    RUN_TEST(test_line_coupler_config_round_trips);
    RUN_TEST(test_group_coupler_config_round_trips);
    RUN_TEST(test_config_decoding_maps_the_named_fields);
    RUN_TEST(test_repetition_follows_the_frame_class);

    return UNITY_END();
}
