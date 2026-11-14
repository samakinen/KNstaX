// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_filter_table_load_control.cpp
 * @brief Filter Table Realisation Type 3 load state machine (03_05_01
 *        §5.3.1, §4.23.2, §4.2.7, §4.2.27).
 */

#include "unity.h"

#include "knx/network/filter_table_load_control.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace knx;
using network::FilterTable;
using network::FilterTableLoadControl;
using State = FilterTableLoadControl::State;

namespace {

std::array<uint8_t, 10> event(uint8_t code, std::initializer_list<uint8_t> data = {})
{
    std::array<uint8_t, 10> block{};
    block[0] = code;
    size_t i = 1;
    for (uint8_t octet : data) {
        block[i++] = octet;
    }
    return block;
}

std::array<uint8_t, 10> dataRelativeAllocation(uint32_t size, bool fill, uint8_t fillValue)
{
    return event(FilterTableLoadControl::kEventAdditionalLoadControls,
                 {FilterTableLoadControl::kDataRelativeAllocation,
                  static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                  static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size),
                  static_cast<uint8_t>(fill ? 1u : 0u), fillValue});
}

FilterTableLoadControl::Mcb mcbFor(uint32_t segmentSize, uint16_t crc)
{
    return FilterTableLoadControl::Mcb{
        static_cast<uint8_t>(segmentSize >> 24), static_cast<uint8_t>(segmentSize >> 16),
        static_cast<uint8_t>(segmentSize >> 8), static_cast<uint8_t>(segmentSize),
        0x00, 0x00, static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// §4.2.27.1.2: "The correct CRC16-CCITT of the string '123456789' is E5CCh."
void test_crc_is_the_specified_ccitt_variant(void)
{
    const char* text = "123456789";
    const auto crc = FilterTableLoadControl::crc16Ccitt(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text), std::strlen(text)));
    TEST_ASSERT_EQUAL_HEX16(0xE5CCu, crc);
}

// §4.5.3: the default Load State is Loaded; §4.2.7: the reference points at
// the table.
void test_defaults_to_loaded_with_a_reference(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    TEST_ASSERT_TRUE(control.loaded());
    TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress, control.tableReference());
}

// The ETS-like sequence of §5.3.1.5 / Figure 64: Unload, Start Loading,
// allocate, write the bit field, write the MCB, Load Completed.
void test_type3_download_with_mcb_loads_the_table(void)
{
    FilterTable table;
    table.setAll(true);
    FilterTableLoadControl control(table);
    std::vector<bool> loadedEvents;
    control.setStateListener([&](bool loaded) { loadedEvents.push_back(loaded); });

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(control.state()));
    TEST_ASSERT_EQUAL_HEX32(0u, control.tableReference());

    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Loading), static_cast<int>(control.state()));
    control.applyEvent(dataRelativeAllocation(FilterTable::kOctets, true, 0x00));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Loading), static_cast<int>(control.state()));
    TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress, control.tableReference());

    // A_Memory_Write into the window: GA 1/2/3 (0A03h) and 2/0/0..2/0/15.
    auto memory = control.memory();
    memory[0x0A03u >> 3] = static_cast<uint8_t>(1u << (0x0A03u & 7u));
    memory[0x1000u >> 3] = 0xFF;
    memory[(0x1000u >> 3) + 1u] = 0xFF;
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(memory);
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, crc)).isOk());

    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_TRUE(control.loaded());
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x0A03)));
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x100F)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x0A04)));
    TEST_ASSERT_EQUAL_UINT32(17u, table.passCount());
    const auto mcb = control.mcb();
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc >> 8), mcb[6]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc), mcb[7]);

    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(loadedEvents.size()));
    TEST_ASSERT_FALSE(loadedEvents[0]); // Unloaded
    TEST_ASSERT_FALSE(loadedEvents[1]); // Loading
    TEST_ASSERT_TRUE(loadedEvents[2]);  // Loaded
}

// Without an MCB the device computes and publishes the CRC of the whole table.
void test_download_without_mcb_publishes_the_crc(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.memory()[3] = 0x10;
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_TRUE(control.loaded());

    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(control.memory());
    const auto mcb = control.mcb();
    TEST_ASSERT_EQUAL_HEX8(0x00, mcb[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, mcb[1]);
    TEST_ASSERT_EQUAL_HEX8(0x20, mcb[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, mcb[3]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc >> 8), mcb[6]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc), mcb[7]);
}

// Table 93 "Load Completed": Loaded only if no error in the data is detected.
void test_crc_mismatch_ends_in_error(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.memory()[100] = 0x01;
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(control.memory());
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, static_cast<uint16_t>(crc ^ 0x0001u))).isOk());

    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));

    // Table 94: Error stays Error on Start Loading and Load Completed; only
    // Unload leaves it.
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));
    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(control.state()));
}

// Unload declares the data invalid (Table 93) and frees the reference (§4.2.7).
void test_unload_clears_the_table_and_the_reference(void)
{
    FilterTable table;
    table.setAll(true);
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    TEST_ASSERT_EQUAL_UINT32(0u, table.passCount());
    TEST_ASSERT_EQUAL_HEX32(0u, control.tableReference());
    const auto mcb = control.mcb();
    for (uint8_t octet : mcb) {
        TEST_ASSERT_EQUAL_HEX8(0u, octet);
    }

    // Load Completed from Unloaded stays Unloaded (Table 94, recommended).
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(control.state()));
    // Loading after Unload without an allocation cannot complete.
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));
}

// 03_05_02 §3.31.3.4: an allocation the device cannot honour -> Error.
void test_oversized_or_misplaced_allocation_is_an_error(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(dataRelativeAllocation(FilterTable::kOctets + 1u, false, 0));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));
    TEST_ASSERT_EQUAL_HEX32(0u, control.tableReference());

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(event(FilterTableLoadControl::kEventAdditionalLoadControls,
                             {FilterTableLoadControl::kAllocAbsDataSeg, 0x40, 0x00, 0x20, 0x00}));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(event(FilterTableLoadControl::kEventAdditionalLoadControls,
                             {FilterTableLoadControl::kAllocAbsDataSeg, 0x80, 0x00, 0x20, 0x00}));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Loading), static_cast<int>(control.state()));
    TEST_ASSERT_EQUAL_HEX32(0x8000u, control.tableReference());
}

// Data Relative Allocation with mode bit 0 fills the allocated memory.
void test_allocation_fill(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(dataRelativeAllocation(FilterTable::kOctets, true, 0xFF));
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_TRUE(control.loaded());
    TEST_ASSERT_EQUAL_UINT32(65535u, table.passCount());
}

// §4.2.27: writing the MCB only has effect while Loading.
void test_mcb_is_written_only_while_loading(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(16, 0x1234)).isError());
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    TEST_ASSERT_TRUE(control.writeMcb(std::array<uint8_t, 7>{}).isError());
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets + 8u, 0)).isOk());
    // A segment larger than the table cannot be checked.
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));
}

// Table 94: Additional Load Controls in Loaded -> Error; unknown events and
// No Operation change nothing; a restart drops an interrupted Loading.
void test_remaining_transitions(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(0x07));
    control.applyEvent(event(FilterTableLoadControl::kEventNoOperation));
    TEST_ASSERT_TRUE(control.loaded());

    control.applyEvent(dataRelativeAllocation(16, false, 0));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.deviceRestart();
    TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(control.state()));
}

// §4.2.28: PID_ERROR_CODE carries the reason for Error and goes back to 0
// when the state leaves Error.
void test_error_code_follows_the_error_state(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorNone, control.errorCode());

    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(control.memory());
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, static_cast<uint16_t>(crc ^ 1u))).isOk());
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorCrc, control.errorCode());

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorNone, control.errorCode());

    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(dataRelativeAllocation(0x2001, false, 0));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorMaxTableLength, control.errorCode());
    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.applyEvent(dataRelativeAllocation(0, false, 0));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorAllocationSizeZero, control.errorCode());
}

namespace {

/// Store @p source's state, restore it into a fresh control, and check the
/// table and the record size.
void roundTrip(FilterTable& source, size_t maxRecordSize)
{
    FilterTableLoadControl from(source);
    std::vector<uint8_t> record;
    from.serialize(record);
    TEST_ASSERT_TRUE(record.size() <= maxRecordSize);
    TEST_ASSERT_TRUE(record.size() <= FilterTableLoadControl::kMaxRecordSize);

    FilterTable restored;
    restored.setAll(true);
    FilterTableLoadControl to(restored);
    to.restore(record);
    TEST_ASSERT_TRUE(to.loaded());
    TEST_ASSERT_EQUAL_MEMORY(source.octets().data(), restored.octets().data(), FilterTable::kOctets);
}

} // namespace

// The stored record is the table run-length encoded, or raw when that is
// shorter, so even the worst case stays one header over 8 KB.
void test_record_round_trips_worst_case_tables(void)
{
    FilterTable table;
    roundTrip(table, FilterTableLoadControl::kRecordHeaderSize + 3u); // empty

    // All set: octet 0 differs (GA 0000h is never set), the rest is one run.
    table.setAll(true);
    roundTrip(table, FilterTableLoadControl::kRecordHeaderSize + 6u);

    // Alternating bits: every octet AAh, except octet 0.
    for (uint32_t ga = 1; ga <= 0xFFFFu; ++ga) {
        table.set(GroupAddress(static_cast<uint16_t>(ga)), (ga & 1u) != 0u);
    }
    roundTrip(table, FilterTableLoadControl::kRecordHeaderSize + 6u);

    // Alternating octets: the worst case for runs, so stored raw.
    for (uint32_t ga = 1; ga <= 0xFFFFu; ++ga) {
        table.set(GroupAddress(static_cast<uint16_t>(ga)), ((ga >> 3) & 1u) != 0u);
    }
    roundTrip(table, FilterTableLoadControl::kMaxRecordSize);

    // Noise: no runs at all.
    uint32_t lcg = 12345u;
    for (auto& octet : table.octets()) {
        lcg = lcg * 1103515245u + 12345u;
        octet = static_cast<uint8_t>(lcg >> 16);
    }
    roundTrip(table, FilterTableLoadControl::kMaxRecordSize);

    // What a download typically holds: a few ranges and single addresses.
    table.setAll(false);
    table.setRange(0x0800, 0x08FF, true);
    table.set(GroupAddress(0x0A03), true);
    table.set(GroupAddress(0x1F11), true);
    roundTrip(table, 64u);
}

// Table 94 "Device Restart": Loaded, Unloaded and Error come back as stored,
// with the allocation, MCB and error code; Loading becomes Unloaded.
void test_restore_applies_device_restart(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    control.memory()[0x0101] = 0x02;
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(control.memory());
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, crc)).isOk());
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_TRUE(control.loaded());
    std::vector<uint8_t> loaded;
    control.serialize(loaded);

    {
        FilterTable restored;
        FilterTableLoadControl after(restored);
        after.restore(loaded);
        TEST_ASSERT_TRUE(after.loaded());
        TEST_ASSERT_TRUE(restored.passes(GroupAddress(0x0809)));
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(restored.passCount()));
        TEST_ASSERT_TRUE(after.mcb() == control.mcb());
        TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress, after.tableReference());
    }

    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    std::vector<uint8_t> loading;
    control.serialize(loading);
    {
        FilterTable restored;
        FilterTableLoadControl after(restored);
        after.restore(loading);
        TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(after.state()));
        TEST_ASSERT_EQUAL_HEX32(0u, after.tableReference());
    }

    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, static_cast<uint16_t>(crc ^ 1u))).isOk());
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    std::vector<uint8_t> error;
    control.serialize(error);
    {
        FilterTable restored;
        FilterTableLoadControl after(restored);
        after.restore(error);
        TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(after.state()));
        TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorCrc, after.errorCode());
    }

    control.applyEvent(event(FilterTableLoadControl::kEventUnload));
    std::vector<uint8_t> unloaded;
    control.serialize(unloaded);
    {
        FilterTable restored;
        FilterTableLoadControl after(restored);
        after.restore(unloaded);
        TEST_ASSERT_EQUAL(static_cast<int>(State::Unloaded), static_cast<int>(after.state()));
        TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorNone, after.errorCode());
    }

    // Nothing stored: the new device of §4.5.3, Loaded and cleared.
    FilterTable fresh;
    FilterTableLoadControl never(fresh);
    never.restore({});
    TEST_ASSERT_TRUE(never.loaded());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(fresh.passCount()));
}

// Table 94: "Error in case of error detection at start-up". A record that
// does not decode, or whose table does not match its CRC, is not trusted.
void test_corrupt_record_restores_as_error(void)
{
    FilterTable table;
    table.setRange(0x0100, 0x01FF, true);
    FilterTableLoadControl control(table);
    std::vector<uint8_t> record;
    control.serialize(record);

    auto flipped = record;
    flipped.back() ^= 0x01u; // the value of the last run
    FilterTable restored;
    FilterTableLoadControl after(restored);
    after.restore(flipped);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(after.state()));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorCrc, after.errorCode());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(restored.passCount()));

    auto truncated = record;
    truncated.pop_back();
    FilterTable restored2;
    FilterTableLoadControl after2(restored2);
    after2.restore(truncated);
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(after2.state()));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorConfiguration, after2.errorCode());

    FilterTable restored3;
    FilterTableLoadControl after3(restored3);
    after3.restoreUnreadable();
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(after3.state()));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorGeneralDevice, after3.errorCode());
}

// A factory reset is the new-device state of §4.5.3, whatever came before.
void test_factory_reset_is_loaded_and_cleared(void)
{
    FilterTable table;
    FilterTableLoadControl control(table);
    control.applyEvent(event(FilterTableLoadControl::kEventStartLoading));
    table.setAll(true);
    TEST_ASSERT_TRUE(control.writeMcb(mcbFor(FilterTable::kOctets, 0)).isOk());
    control.applyEvent(event(FilterTableLoadControl::kEventLoadCompleted));
    TEST_ASSERT_EQUAL(static_cast<int>(State::Error), static_cast<int>(control.state()));

    control.factoryReset();
    TEST_ASSERT_TRUE(control.loaded());
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorNone, control.errorCode());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(table.passCount()));
    TEST_ASSERT_TRUE(control.mcb() == FilterTableLoadControl::Mcb{});
    TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress, control.tableReference());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc_is_the_specified_ccitt_variant);
    RUN_TEST(test_defaults_to_loaded_with_a_reference);
    RUN_TEST(test_type3_download_with_mcb_loads_the_table);
    RUN_TEST(test_download_without_mcb_publishes_the_crc);
    RUN_TEST(test_crc_mismatch_ends_in_error);
    RUN_TEST(test_unload_clears_the_table_and_the_reference);
    RUN_TEST(test_oversized_or_misplaced_allocation_is_an_error);
    RUN_TEST(test_allocation_fill);
    RUN_TEST(test_mcb_is_written_only_while_loading);
    RUN_TEST(test_remaining_transitions);
    RUN_TEST(test_error_code_follows_the_error_state);
    RUN_TEST(test_record_round_trips_worst_case_tables);
    RUN_TEST(test_restore_applies_device_restart);
    RUN_TEST(test_corrupt_record_restores_as_error);
    RUN_TEST(test_factory_reset_is_loaded_and_cleared);
    return UNITY_END();
}
