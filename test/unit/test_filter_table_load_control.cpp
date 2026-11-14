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
    return UNITY_END();
}
