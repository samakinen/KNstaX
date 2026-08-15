// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_group_object_flags.cpp
 * @brief Unit tests for the KNX group object communication flags (C/R/W/T/U/I).
 *
 * These flags are the ones ETS shows on a communication object.  The point of
 * these tests is that they are *enforced*, not merely declared: a telegram a
 * flag forbids must not reach the object's value or the firmware callback.
 */

#include "unity.h"
#include "knx/application/group_object.hpp"

#include <array>

using namespace knx;
using namespace knx::application;

namespace {

GroupObjectConfig makeConfig(const GroupObjectFlags& flags) {
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = dptids::Switch;
    config.flags = flags;
    return config;
}

constexpr std::array<uint8_t, 1> kOne{0x01};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Write enable (W) ------------------------------------------------------

void test_write_enable_accepts_group_value_write(void) {
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.write = true;
    GroupObject go(makeConfig(flags));

    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Write).isOk());
    TEST_ASSERT_TRUE(go.asBool());
}

void test_write_disabled_rejects_group_value_write(void) {
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.write = false;
    GroupObject go(makeConfig(flags));

    const auto res = go.receiveValue(kOne, GroupObject::ValueSource::Write);
    TEST_ASSERT_TRUE(res.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::AccessDenied), static_cast<int>(res.error()));
    // The value must be untouched, not merely "not reported".
    TEST_ASSERT_FALSE(go.isValid());
}

// --- Response-Update enable (U) is distinct from W -------------------------

void test_update_enable_governs_response_not_write(void) {
    // Write-only object: accepts commands, ignores other devices' responses.
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.write = true;
    flags.update = false;
    GroupObject go(makeConfig(flags));

    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Write).isOk());
    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Response).isError());
}

void test_response_update_accepts_response(void) {
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.write = false;
    flags.update = true;
    GroupObject go(makeConfig(flags));

    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Response).isOk());
    TEST_ASSERT_TRUE(go.asBool());
    // ...and still refuses a write, because W is clear.
    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Write).isError());
}

// --- Communication enable (C) overrides everything -------------------------

void test_communication_disabled_blocks_all_directions(void) {
    GroupObjectFlags flags{};
    flags.communication = false;
    flags.read = true;
    flags.write = true;
    flags.transmit = true;
    flags.update = true;
    flags.readOnInit = true;
    GroupObject go(makeConfig(flags));

    TEST_ASSERT_FALSE(go.readable());
    TEST_ASSERT_FALSE(go.writable());
    TEST_ASSERT_FALSE(go.transmit());
    TEST_ASSERT_FALSE(go.updatable());
    TEST_ASSERT_FALSE(go.readOnInit());

    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Write).isError());
    TEST_ASSERT_TRUE(go.receiveValue(kOne, GroupObject::ValueSource::Response).isError());
    // requestValue() is gated by C as well.
    TEST_ASSERT_TRUE(go.requestValue().isError());
}

// --- Value Field Type derivation ------------------------------------------

void test_value_field_type_tracks_dpt_width(void) {
    GroupObjectFlags flags{};
    flags.communication = true;

    GroupObjectConfig oneBit{};
    oneBit.address = GroupAddress(1, 1, 1);
    oneBit.dpt = dptids::Switch;  // DPT 1 → 1 bit → code 0
    oneBit.flags = flags;
    TEST_ASSERT_EQUAL(0u, GroupObject(oneBit).valueFieldType());

    GroupObjectConfig dimming{};
    dimming.address = GroupAddress(1, 1, 2);
    dimming.dpt = dptids::Dimming;  // DPT 3 → 4 bit → code 3
    dimming.flags = flags;
    TEST_ASSERT_EQUAL(3u, GroupObject(dimming).valueFieldType());

    GroupObjectConfig scaling{};
    scaling.address = GroupAddress(1, 1, 3);
    scaling.dpt = dptids::Unsigned8;  // DPT 5 → 1 octet → code 7
    scaling.flags = flags;
    TEST_ASSERT_EQUAL(7u, GroupObject(scaling).valueFieldType());

    GroupObjectConfig temperature{};
    temperature.address = GroupAddress(1, 1, 4);
    temperature.dpt = dptids::Float2Byte;  // DPT 9 → 2 octets → code 8
    temperature.flags = flags;
    TEST_ASSERT_EQUAL(8u, GroupObject(temperature).valueFieldType());
}

// --- Descriptor reflects the live flags ------------------------------------

void test_descriptor_reflects_flags_and_width(void) {
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.read = true;
    flags.transmit = true;
    flags.priority = Priority::Urgent;

    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = dptids::Float2Byte;  // 2 octets → Value Field Type 8
    config.flags = flags;
    GroupObject go(config);

    uint8_t valueFieldType = 0;
    const auto decoded = decodeGroupObjectDescriptor(go.descriptor(), valueFieldType);

    TEST_ASSERT_EQUAL(8u, valueFieldType);
    TEST_ASSERT_TRUE(decoded.communication);
    TEST_ASSERT_TRUE(decoded.read);
    TEST_ASSERT_TRUE(decoded.transmit);
    TEST_ASSERT_FALSE(decoded.write);
    TEST_ASSERT_FALSE(decoded.update);
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::Urgent), static_cast<int>(decoded.priority));

    // setFlags() must be visible in the descriptor: this is the path an ETS
    // download uses, so a stale descriptor would mean the device reports one
    // configuration while behaving as another.
    GroupObjectFlags changed = flags;
    changed.write = true;
    changed.readOnInit = true;
    go.setFlags(changed);

    const auto reDecoded = decodeGroupObjectDescriptor(go.descriptor(), valueFieldType);
    TEST_ASSERT_TRUE(reDecoded.write);
    TEST_ASSERT_TRUE(reDecoded.readOnInit);
}

// --- Footprint -------------------------------------------------------------

void test_group_object_footprint_stays_bounded(void) {
    // A device with dozens of communication objects pays sizeof(GroupObject)
    // for each one, and the object holds two payload buffers. Sizing those for
    // a 254-octet extended APDU cost ~500 bytes per object of RAM that a 1- or
    // 2-byte datapoint never uses. This guard exists so that regression cannot
    // creep back in unnoticed.
    TEST_ASSERT_TRUE(sizeof(GroupObject) <= 320);

    // And the bound must still cover every datapoint the stack claims to
    // support — the two constraints pull in opposite directions, so both are
    // asserted together.
    TEST_ASSERT_TRUE(config::MAX_GROUP_OBJECT_PAYLOAD_BYTES >= maxCatalogPayloadOctets());
}

void test_oversized_payload_is_rejected_not_truncated(void) {
    // Group objects used as opaque transport can exceed the bound. Silently
    // storing a truncated value would be far worse than refusing it.
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.write = true;
    GroupObject go(makeConfig(flags));

    std::array<uint8_t, config::MAX_GROUP_OBJECT_PAYLOAD_BYTES + 1> tooBig{};
    const auto res = go.receiveValue(tooBig, GroupObject::ValueSource::Write);
    TEST_ASSERT_TRUE(res.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::BufferTooSmall),
                      static_cast<int>(res.error()));
    TEST_ASSERT_FALSE(go.isValid());
}

void test_widest_catalog_dpt_still_fits(void) {
    // DPT 16 (14-octet string) is the widest entry; it must round-trip.
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = dptids::Label;
    config.flags.communication = true;
    config.flags.write = true;
    GroupObject go(config);

    std::array<uint8_t, 14> label{'K','N','s','t','a','X',0,0,0,0,0,0,0,0};
    TEST_ASSERT_TRUE(go.receiveValue(label, GroupObject::ValueSource::Write).isOk());
    TEST_ASSERT_EQUAL(14u, go.getRawValue().size());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_group_object_footprint_stays_bounded);
    RUN_TEST(test_oversized_payload_is_rejected_not_truncated);
    RUN_TEST(test_widest_catalog_dpt_still_fits);
    RUN_TEST(test_write_enable_accepts_group_value_write);
    RUN_TEST(test_write_disabled_rejects_group_value_write);
    RUN_TEST(test_update_enable_governs_response_not_write);
    RUN_TEST(test_response_update_accepts_response);
    RUN_TEST(test_communication_disabled_blocks_all_directions);
    RUN_TEST(test_value_field_type_tracks_dpt_width);
    RUN_TEST(test_descriptor_reflects_flags_and_width);
    return UNITY_END();
}
