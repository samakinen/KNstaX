// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_filter_table_type3_download.cpp
 * @brief A Filter Table Realisation Type 3 download through the real stack.
 *
 * The sequence 03/05/01 §5.3.1.5 and Figure 64 (§4.23.2.4.1) describe, as
 * telegrams on a transport connection: Unload, Start Loading, Data Relative
 * Allocation (03/05/02 §3.31.3.4), PID_TABLE_REFERENCE read, A_Memory_Write
 * into the window, PID_MCB_TABLE write, Load Completed. Which of this ETS
 * actually sends for a coupler has not been captured on the bench yet.
 */

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/bau/bau.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/network/filter_table_load_control.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::test;
using network::FilterTable;
using network::FilterTableLoadControl;

namespace {

constexpr IndividualAddress kCoupler(1, 1, 0);
constexpr IndividualAddress kEts(0, 0, 1);
constexpr uint8_t kPidLoadStateControl = 5;
constexpr uint8_t kPidTableReference = 7;
constexpr uint8_t kPidMcbTable = 27;

MockPhysicalLayer* gPhysical = nullptr;
std::unique_ptr<platform::LinuxPlatform> gPlatform;
std::unique_ptr<bau::BusAccessUnit> gBau;
FilterTable gTable;
uint8_t gSeq = 0;
uint8_t gRouterIndex = 0;

struct Sent {
    protocol::TPCI tpci{};
    uint8_t seq{0};
    uint16_t apci{0};
    std::vector<uint8_t> payload;
};

void clearPersistenceStore()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir("knx_objects"), ec);
}

void inject(std::span<const uint8_t> tpdu)
{
    datalink::LDataFrame frame;
    frame.source = kEts;
    frame.destination = GroupAddress(kCoupler.raw);
    frame.destinationType = AddressType::Individual;
    frame.priority = Priority::System;
    frame.hopCount = 6;
    frame.tpdu.assign(tpdu);
    frame.standardFrame = tpdu.size() <= 16u;
    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE> buffer{};
    auto encoded = datalink::FrameCodec::encodeFrame(frame, buffer);
    TEST_ASSERT_TRUE(encoded.isOk());
    gPhysical->injectFrame(std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value()));
}

std::vector<Sent> pump(int budgetMs = 40)
{
    std::vector<Sent> sent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        gBau->loop();
        std::vector<uint8_t> wire;
        while (gPhysical->getSentFrame(wire)) {
            datalink::LDataFrame decoded;
            if (datalink::FrameCodec::decodeFrame(wire, decoded).isError() || decoded.tpdu.empty()) {
                continue;
            }
            const auto header = protocol::unpackTpduHeader(decoded.tpdu[0],
                                                           decoded.tpdu.size() > 1u ? decoded.tpdu[1] : 0u);
            sent.push_back(Sent{header.tpci.type(), header.tpci.seqNum(), header.apci.raw,
                                std::vector<uint8_t>(decoded.tpdu.begin() + (decoded.tpdu.size() > 2u ? 2 : decoded.tpdu.size()),
                                                     decoded.tpdu.end())});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return sent;
}

/// One request on the connection; acknowledges the device's answer and
/// returns the payload of its A_PropertyValue_Response / A_Memory_Response.
std::optional<std::vector<uint8_t>> request(application::APCIService service, uint8_t data6,
                                            std::vector<uint8_t> payload)
{
    const auto tpdu = protocol::buildTpdu(protocol::TPCIField::create(protocol::TPCI::NumberedData, gSeq),
                                          application::APCIField::create(service, data6),
                                          std::span<const uint8_t>(payload));
    gSeq = static_cast<uint8_t>((gSeq + 1u) & 0x0Fu);
    inject(std::vector<uint8_t>(tpdu.begin(), tpdu.end()));
    const auto sent = pump();
    std::optional<std::vector<uint8_t>> answer;
    for (const auto& frame : sent) {
        if (frame.tpci == protocol::TPCI::NumberedData) {
            const auto ack = protocol::buildTpdu(protocol::TPCIField::ack(frame.seq), application::APCIField(0),
                                                 std::span<const uint8_t>{});
            inject(std::vector<uint8_t>(ack.begin(), ack.end()));
            (void)pump(10);
            answer = frame.payload;
        }
    }
    return answer;
}

std::vector<uint8_t> propertyRead(uint8_t objectIndex, uint8_t pid)
{
    auto answer = request(application::APCIService::PropertyValueRead, 0, {objectIndex, pid, 0x10, 0x01});
    TEST_ASSERT_TRUE(answer.has_value());
    TEST_ASSERT_TRUE(answer->size() >= 4u);
    return std::vector<uint8_t>(answer->begin() + 4, answer->end());
}

/// A_PropertyValue_Write of one element; returns the response's data.
std::vector<uint8_t> propertyWrite(uint8_t pid, std::vector<uint8_t> value)
{
    std::vector<uint8_t> payload{gRouterIndex, pid, 0x10, 0x01};
    payload.insert(payload.end(), value.begin(), value.end());
    auto answer = request(application::APCIService::PropertyValueWrite, 0, payload);
    TEST_ASSERT_TRUE(answer.has_value());
    TEST_ASSERT_TRUE(answer->size() >= 4u);
    return std::vector<uint8_t>(answer->begin() + 4, answer->end());
}

uint8_t loadEvent(std::vector<uint8_t> block)
{
    block.resize(10, 0x00);
    const auto state = propertyWrite(kPidLoadStateControl, block);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(state.size()));
    return state[0];
}

void memoryWrite(uint16_t address, std::vector<uint8_t> data)
{
    std::vector<uint8_t> payload{static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
    payload.insert(payload.end(), data.begin(), data.end());
    (void)request(application::APCIService::MemoryWrite, static_cast<uint8_t>(data.size()), payload);
}

uint32_t be32(const std::vector<uint8_t>& v)
{
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(v.size()));
    return (static_cast<uint32_t>(v[0]) << 24) | (static_cast<uint32_t>(v[1]) << 16)
         | (static_cast<uint32_t>(v[2]) << 8) | v[3];
}

/// Unload, Start Loading, allocate 8 KB zero-filled, write two runs of bits.
/// Returns the table the device should now hold.
FilterTable loadTableBody()
{
    TEST_ASSERT_EQUAL_UINT8(0u, loadEvent({0x04}));                       // Unloaded
    TEST_ASSERT_EQUAL_HEX32(0u, be32(propertyRead(gRouterIndex, kPidTableReference)));
    TEST_ASSERT_EQUAL_UINT8(2u, loadEvent({0x01}));                       // Loading
    TEST_ASSERT_EQUAL_UINT8(2u, loadEvent({0x03, 0x0B, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00}));
    const uint32_t reference = be32(propertyRead(gRouterIndex, kPidTableReference));
    TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress, reference);

    FilterTable expected;
    // GA 1/2/3 (0A03h): octet 0140h, bit 3.
    expected.set(GroupAddress(0x0A03), true);
    memoryWrite(static_cast<uint16_t>(reference + (0x0A03u >> 3)), {0x08});
    // GAs 2/0/0..2/0/95 (1000h..105Fh): twelve octets of FFh.
    expected.setRange(0x1000, 0x105F, true);
    memoryWrite(static_cast<uint16_t>(reference + (0x1000u >> 3)), std::vector<uint8_t>(12, 0xFF));
    return expected;
}

std::vector<uint8_t> mcb(uint16_t crc)
{
    return {0x00, 0x00, 0x20, 0x00, 0x00, 0x00, static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
}

} // namespace

void setUp(void)
{
    clearPersistenceStore();
    gTable = FilterTable{};
    gSeq = 0;
    gPlatform = std::make_unique<platform::LinuxPlatform>();
    auto physical = std::make_unique<MockPhysicalLayer>();
    gPhysical = physical.get();
    gBau = std::make_unique<bau::BusAccessUnit>(*gPlatform, createTp1TestStackPort(*gPlatform, std::move(physical)));
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gTable;
    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isOk());
    TEST_ASSERT_TRUE(gBau->init(kCoupler).isOk());
    gBau->link().setPromiscuousMode(datalink::PromiscuousMode::Enable);
    (void)pump(20);

    inject(std::vector<uint8_t>{0x80}); // T_Connect
    (void)pump(20);

    // Find the Router Object (PID_OBJECT_TYPE = 6) by index, as ETS does.
    gRouterIndex = 0xFF;
    for (uint8_t index = 0; index < 24u && gRouterIndex == 0xFF; ++index) {
        auto answer = request(application::APCIService::PropertyValueRead, 0, {index, 0x01, 0x10, 0x01});
        if (answer.has_value() && answer->size() == 6u && (*answer)[4] == 0x00 && (*answer)[5] == 0x06) {
            gRouterIndex = index;
        }
    }
    TEST_ASSERT_NOT_EQUAL(0xFF, gRouterIndex);
}

void tearDown(void)
{
    if (gBau) {
        gBau->close();
        gBau.reset();
    }
    gPhysical = nullptr;
    gPlatform.reset();
    clearPersistenceStore();
}

void test_type3_download_with_matching_mcb_loads_the_table(void)
{
    gTable.setAll(true); // whatever was there before the download
    const FilterTable expected = loadTableBody();
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(expected.octets());
    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(propertyWrite(kPidMcbTable, mcb(crc)).size()));

    TEST_ASSERT_EQUAL_UINT8(1u, loadEvent({0x02}));                       // Loaded
    TEST_ASSERT_EQUAL_UINT32(expected.passCount(), gTable.passCount());
    TEST_ASSERT_TRUE(gTable.passes(GroupAddress(0x0A03)));
    TEST_ASSERT_TRUE(gTable.passes(GroupAddress(0x105F)));
    TEST_ASSERT_FALSE(gTable.passes(GroupAddress(0x1060)));
    const auto readBack = propertyRead(gRouterIndex, kPidMcbTable);
    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(readBack.size()));
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc >> 8), readBack[6]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(crc), readBack[7]);
    // A plain read of the load state returns the state.
    TEST_ASSERT_EQUAL_UINT8(1u, propertyRead(gRouterIndex, kPidLoadStateControl).at(0));
}

void test_type3_download_with_wrong_crc_ends_in_error(void)
{
    const FilterTable expected = loadTableBody();
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(expected.octets());
    (void)propertyWrite(kPidMcbTable, mcb(static_cast<uint16_t>(crc + 1u)));
    TEST_ASSERT_EQUAL_UINT8(3u, loadEvent({0x02}));                       // Error
    TEST_ASSERT_EQUAL_UINT8(3u, propertyRead(gRouterIndex, kPidLoadStateControl).at(0));
}

void test_unload_clears_the_table(void)
{
    gTable.setAll(true);
    TEST_ASSERT_EQUAL_UINT8(0u, loadEvent({0x04}));
    TEST_ASSERT_EQUAL_UINT32(0u, gTable.passCount());
    TEST_ASSERT_EQUAL_HEX32(0u, be32(propertyRead(gRouterIndex, kPidTableReference)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_type3_download_with_matching_mcb_loads_the_table);
    RUN_TEST(test_type3_download_with_wrong_crc_ends_in_error);
    RUN_TEST(test_unload_clears_the_table);
    return UNITY_END();
}
