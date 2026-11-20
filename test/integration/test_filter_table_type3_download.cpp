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
#include "knx/network/coupler_routing.hpp"
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
/// A Master Reset ends in a restart, which the plain host platform carries out
/// by ending the process.
class RecordingPlatform final : public platform::LinuxPlatform {
public:
    void restart() override { restartRequested = true; }
    bool restartRequested{false};
};

std::unique_ptr<RecordingPlatform> gPlatform;
std::unique_ptr<bau::BusAccessUnit> gBau;
FilterTable gTable;
/// The routing policy the Router Object's coupler configuration is applied
/// to, as a coupler's forwarding path holds it. A new one per boot.
std::unique_ptr<network::CouplerRoutingPolicy> gPolicy;
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
    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE + 8u> buffer{};  // TPDU capacity + header + checksum
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
/// returns it.
std::optional<Sent> requestFrame(application::APCIService service, uint8_t data6,
                                 std::vector<uint8_t> payload)
{
    const auto tpdu = protocol::buildTpdu(protocol::TPCIField::create(protocol::TPCI::NumberedData, gSeq),
                                          application::APCIField::create(service, data6),
                                          std::span<const uint8_t>(payload));
    gSeq = static_cast<uint8_t>((gSeq + 1u) & 0x0Fu);
    inject(std::vector<uint8_t>(tpdu.begin(), tpdu.end()));
    const auto sent = pump();
    std::optional<Sent> answer;
    for (const auto& frame : sent) {
        if (frame.tpci == protocol::TPCI::NumberedData) {
            const auto ack = protocol::buildTpdu(protocol::TPCIField::ack(frame.seq), application::APCIField(0),
                                                 std::span<const uint8_t>{});
            inject(std::vector<uint8_t>(ack.begin(), ack.end()));
            (void)pump(10);
            answer = frame;
        }
    }
    return answer;
}

/// requestFrame(), the payload of the A_PropertyValue_Response /
/// A_Memory_Response only.
std::optional<std::vector<uint8_t>> request(application::APCIService service, uint8_t data6,
                                            std::vector<uint8_t> payload)
{
    auto answer = requestFrame(service, data6, std::move(payload));
    if (!answer.has_value()) {
        return std::nullopt;
    }
    return std::move(answer->payload);
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

/// A_Memory_Write; returns the device's answer, which there is only in
/// Verify Mode (03/03/07 §3.5.4).
std::optional<Sent> memoryWrite(uint16_t address, std::vector<uint8_t> data)
{
    std::vector<uint8_t> payload{static_cast<uint8_t>(address >> 8), static_cast<uint8_t>(address)};
    payload.insert(payload.end(), data.begin(), data.end());
    return requestFrame(application::APCIService::MemoryWrite, static_cast<uint8_t>(data.size()), payload);
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
    (void)memoryWrite(static_cast<uint16_t>(reference + (0x0A03u >> 3)), {0x08});
    // GAs 2/0/0..2/0/95 (1000h..105Fh): twelve octets of FFh.
    expected.setRange(0x1000, 0x105F, true);
    (void)memoryWrite(static_cast<uint16_t>(reference + (0x1000u >> 3)), std::vector<uint8_t>(12, 0xFF));
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
    gPolicy = std::make_unique<network::CouplerRoutingPolicy>();
    gSeq = 0;
    gPlatform = std::make_unique<RecordingPlatform>();
    auto physical = std::make_unique<MockPhysicalLayer>();
    gPhysical = physical.get();
    gBau = std::make_unique<bau::BusAccessUnit>(*gPlatform, createTp1TestStackPort(*gPlatform, std::move(physical)));
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gTable;
    config.routingPolicy = gPolicy.get();
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

namespace {

constexpr uint8_t kPidErrorCode = 28;

/// A_Restart Master Reset, connectionless, as DM_Restart_RCl sends it.
void masterReset(uint8_t eraseCode)
{
    const std::array<uint8_t, 2> data{eraseCode, 0x00};
    const auto tpdu = protocol::buildTpdu(protocol::TPCIField::create(protocol::TPCI::UnnumberedData, 0),
                                          application::APCIField::create(application::APCIService::Restart, 0x01),
                                          std::span<const uint8_t>(data));
    inject(std::vector<uint8_t>(tpdu.begin(), tpdu.end()));
    (void)pump(60);
    TEST_ASSERT_TRUE(gPlatform->restartRequested);
    gPlatform->restartRequested = false;
}

/// What the coupler boots into: a fresh BAU on the same persisted state and
/// a table object that starts out empty.
void restartDevice()
{
    gBau->close();
    gBau.reset();
    gTable = FilterTable{};
    gPolicy = std::make_unique<network::CouplerRoutingPolicy>();
    gPlatform = std::make_unique<RecordingPlatform>();
    auto physical = std::make_unique<MockPhysicalLayer>();
    gPhysical = physical.get();
    gBau = std::make_unique<bau::BusAccessUnit>(*gPlatform, createTp1TestStackPort(*gPlatform, std::move(physical)));
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gTable;
    config.routingPolicy = gPolicy.get();
    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isOk());
    TEST_ASSERT_TRUE(gBau->init(kCoupler).isOk());
    gBau->link().setPromiscuousMode(datalink::PromiscuousMode::Enable);
    (void)pump(20);
    gSeq = 0;
    inject(std::vector<uint8_t>{0x80}); // T_Connect
    (void)pump(20);
}

void downloadWithMatchingMcb(const FilterTable& expected)
{
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(expected.octets());
    (void)propertyWrite(kPidMcbTable, mcb(crc));
    TEST_ASSERT_EQUAL_UINT8(1u, loadEvent({0x02}));
}

} // namespace

// PID_ERROR_CODE (03/05/01 §4.2.28): readable over the bus after a CRC
// mismatch (DPT_ErrorClass_System 9, "CRC-error"), back to 0 once Unload
// leaves Error.
void test_error_code_reads_crc_error_after_a_mismatch(void)
{
    TEST_ASSERT_EQUAL_UINT8(0u, propertyRead(gRouterIndex, kPidErrorCode).at(0));
    const FilterTable expected = loadTableBody();
    const uint16_t crc = FilterTableLoadControl::crc16Ccitt(expected.octets());
    (void)propertyWrite(kPidMcbTable, mcb(static_cast<uint16_t>(crc ^ 0x8000u)));
    TEST_ASSERT_EQUAL_UINT8(3u, loadEvent({0x02}));
    TEST_ASSERT_EQUAL_UINT8(FilterTableLoadControl::kErrorCrc, propertyRead(gRouterIndex, kPidErrorCode).at(0));
    TEST_ASSERT_EQUAL_UINT8(0u, loadEvent({0x04}));
    TEST_ASSERT_EQUAL_UINT8(0u, propertyRead(gRouterIndex, kPidErrorCode).at(0));
}

// The downloaded table and Loaded come back after the restart that ends an
// ETS download, and the Router Object reports them.
void test_downloaded_table_survives_restart(void)
{
    const FilterTable expected = loadTableBody();
    downloadWithMatchingMcb(expected);
    const auto mcbBefore = propertyRead(gRouterIndex, kPidMcbTable);
    gBau->flushPendingPersistence(); // what the A_Restart handler does

    restartDevice();
    TEST_ASSERT_EQUAL_MEMORY(expected.octets().data(), gTable.octets().data(), FilterTable::kOctets);
    TEST_ASSERT_EQUAL_UINT8(1u, propertyRead(gRouterIndex, kPidLoadStateControl).at(0));
    TEST_ASSERT_EQUAL_HEX32(FilterTableLoadControl::kDefaultMemoryAddress,
                            be32(propertyRead(gRouterIndex, kPidTableReference)));
    TEST_ASSERT_TRUE(propertyRead(gRouterIndex, kPidMcbTable) == mcbBefore);
}

// 03/05/02 Table 4: Factory Reset (02h) and Factory Reset without IA (07h)
// reset the device to its ex-factory state, which for the Router Object is
// 03/05/01 §4.5.3 "Loaded and the Routing Table cleared". ResetLinks (06h)
// covers the Group Address and Association Tables only and leaves it.
void test_master_reset_factory_reset_clears_the_filter_table(void)
{
    const FilterTable expected = loadTableBody();
    downloadWithMatchingMcb(expected);
    gBau->flushPendingPersistence();

    masterReset(0x06);
    TEST_ASSERT_EQUAL_UINT32(expected.passCount(), gTable.passCount());

    masterReset(0x02);
    auto* load = gBau->management().filterTableLoadControl();
    TEST_ASSERT_NOT_NULL(load);
    TEST_ASSERT_TRUE(load->loaded());
    TEST_ASSERT_EQUAL_UINT32(0u, gTable.passCount());
    TEST_ASSERT_TRUE(load->mcb() == FilterTableLoadControl::Mcb{});

    // Persisted by the reset itself: the device boots into it.
    restartDevice();
    TEST_ASSERT_EQUAL_UINT32(0u, gTable.passCount());
    TEST_ASSERT_EQUAL_UINT8(1u, propertyRead(gRouterIndex, kPidLoadStateControl).at(0));
}

// 03/05/01 §5.3.1.5: the MaC "shall respect the Load State Machine if
// modifying the Filter Table", i.e. writes it between Start Loading and Load
// Completed (§4.23.2.4, Figure 64). A write into the window in any other
// state is refused like any failed write (03/03/07 §3.5.4): A_Memory_Response
// with number = 0 in Verify Mode, nothing otherwise. Before, a write while
// Loaded changed the live table behind PID_MCB_TABLE's CRC and was not saved.
void test_memory_write_outside_loading_is_refused(void)
{
    const FilterTable expected = loadTableBody();
    downloadWithMatchingMcb(expected);
    const uint16_t address = static_cast<uint16_t>(FilterTableLoadControl::kDefaultMemoryAddress + (0x2000u >> 3));

    // Loaded, Verify Mode off: no answer and no change.
    TEST_ASSERT_FALSE(memoryWrite(address, {0xFF}).has_value());
    TEST_ASSERT_EQUAL_UINT32(expected.passCount(), gTable.passCount());
    TEST_ASSERT_FALSE(gTable.passes(GroupAddress(0x2001)));

    // Verify Mode on: the refusal is A_Memory_Response with number = 0.
    gBau->deviceObject().setDeviceControl(0x04);
    const auto refused = memoryWrite(address, {0xFF});
    TEST_ASSERT_TRUE(refused.has_value());
    TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(application::APCIService::MemoryResponse),
                            static_cast<uint16_t>(refused->apci & 0x3C0u));
    TEST_ASSERT_EQUAL_UINT8(0u, static_cast<uint8_t>(refused->apci & 0x3Fu));
    TEST_ASSERT_EQUAL_UINT32(expected.passCount(), gTable.passCount());
    TEST_ASSERT_EQUAL_UINT8(1u, loadEvent({0x00}));                       // still Loaded

    // Unloaded: no memory is allocated (PID_TABLE_REFERENCE reads 0).
    TEST_ASSERT_EQUAL_UINT8(0u, loadEvent({0x04}));
    const auto unloaded = memoryWrite(address, {0xFF});
    TEST_ASSERT_TRUE(unloaded.has_value());
    TEST_ASSERT_EQUAL_UINT8(0u, static_cast<uint8_t>(unloaded->apci & 0x3Fu));
    TEST_ASSERT_EQUAL_UINT32(0u, gTable.passCount());

    // Loading: accepted and read back.
    TEST_ASSERT_EQUAL_UINT8(2u, loadEvent({0x01}));
    TEST_ASSERT_EQUAL_UINT8(2u, loadEvent({0x03, 0x0B, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00}));
    const auto accepted = memoryWrite(address, {0xFF});
    TEST_ASSERT_TRUE(accepted.has_value());
    TEST_ASSERT_EQUAL_UINT8(1u, static_cast<uint8_t>(accepted->apci & 0x3Fu));
    TEST_ASSERT_TRUE(gTable.passes(GroupAddress(0x2007)));
}

namespace {

constexpr uint8_t kPidMainLcConfig = 52;
constexpr uint8_t kPidSubLcConfig = 53;
constexpr uint8_t kPidMainLcGrpConfig = 54;
constexpr uint8_t kPidSubLcGrpConfig = 55;
constexpr uint8_t kPidFilterTableUse = 67;

/// The coupler configuration of a line coupler that keeps its line to itself:
/// PHYS_LOCK on the main side (76h), broadcasts locked on the sub side (7Fh),
/// GROUP_LOCK6FFF on the sub side (16h), Filter Table not used.
void downloadRoutingConfiguration()
{
    TEST_ASSERT_EQUAL_HEX8(0x76u, propertyWrite(kPidMainLcConfig, {0x76}).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, propertyWrite(kPidSubLcConfig, {0x7F}).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x16u, propertyWrite(kPidSubLcGrpConfig, {0x16}).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x00u, propertyWrite(kPidFilterTableUse, {0x00}).at(0));
}

/// The Router Object's value, read locally: after a Master Reset the
/// transport connection is gone until the device has restarted.
uint8_t routerByte(uint8_t pid)
{
    const auto value = gBau->management().referenceObjectPropertyValue(
        InterfaceObjectType::router(), static_cast<application::PropertyID>(pid));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(value.size()));
    return value[0];
}

void assertDefaultRoutingConfiguration()
{
    // 03/05/01 §4.5.5 / §4.5.6 "(d)" values; PID_FILTER_TABLE_USE as the
    // product declared it (RouterRoleConfig default: in use).
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultLineCouplerConfig, routerByte(kPidMainLcConfig));
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultLineCouplerConfig, routerByte(kPidSubLcConfig));
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultGroupCouplerConfig, routerByte(kPidMainLcGrpConfig));
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultGroupCouplerConfig, routerByte(kPidSubLcGrpConfig));
    TEST_ASSERT_EQUAL_HEX8(0x01u, routerByte(kPidFilterTableUse));
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultLineCouplerConfig,
                           gPolicy->lineConfig(network::CouplerPort::Primary).encode());
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultLineCouplerConfig,
                           gPolicy->lineConfig(network::CouplerPort::Secondary).encode());
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultGroupCouplerConfig,
                           gPolicy->groupConfig(network::CouplerPort::Secondary).encode());
    TEST_ASSERT_TRUE(gPolicy->isFilterTableInUse());
}

} // namespace

// The coupler configuration ETS downloads into the Router Object (03/05/01
// §4.5.5, §4.5.6, §4.5.13) survives the restart that ends the download, and
// the forwarding path uses it from the first frame after it. Before, it was
// not persisted and the seeding at start-up overwrote it with the defaults.
void test_routing_configuration_survives_restart(void)
{
    assertDefaultRoutingConfiguration();
    downloadRoutingConfiguration();
    // Only what the writes marked dirty is saved: this is the A_Restart flush.
    gBau->flushPendingPersistence();

    restartDevice();
    TEST_ASSERT_EQUAL_HEX8(0x76u, propertyRead(gRouterIndex, kPidMainLcConfig).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, propertyRead(gRouterIndex, kPidSubLcConfig).at(0));
    TEST_ASSERT_EQUAL_HEX8(network::kDefaultGroupCouplerConfig, propertyRead(gRouterIndex, kPidMainLcGrpConfig).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x16u, propertyRead(gRouterIndex, kPidSubLcGrpConfig).at(0));
    TEST_ASSERT_EQUAL_HEX8(0x00u, propertyRead(gRouterIndex, kPidFilterTableUse).at(0));

    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Lock),
                      static_cast<int>(gPolicy->lineConfig(network::CouplerPort::Primary).physFrame));
    TEST_ASSERT_EQUAL_HEX8(0x7Fu, gPolicy->lineConfig(network::CouplerPort::Secondary).encode());
    TEST_ASSERT_EQUAL(static_cast<int>(network::GroupHandling::Lock),
                      static_cast<int>(gPolicy->groupConfig(network::CouplerPort::Secondary).group6FFF));
    TEST_ASSERT_EQUAL(static_cast<int>(network::GroupHandling::Route),
                      static_cast<int>(gPolicy->groupConfig(network::CouplerPort::Primary).group6FFF));
    TEST_ASSERT_FALSE(gPolicy->isFilterTableInUse());
}

// Master Reset 02h and 07h return the device to its ex-factory state (03/05/02
// Table 4), and Table 6 names nothing of a coupler's that 07h keeps: the
// coupler configuration goes back to its defaults, in the policy at once and
// in what the next boot restores. 06h (links only) leaves it.
void test_factory_reset_restores_the_default_routing_configuration(void)
{
    downloadRoutingConfiguration();
    gBau->flushPendingPersistence();

    masterReset(0x06);
    TEST_ASSERT_EQUAL_HEX8(0x76u, routerByte(kPidMainLcConfig));
    TEST_ASSERT_EQUAL_HEX8(0x00u, routerByte(kPidFilterTableUse));

    masterReset(0x07);
    assertDefaultRoutingConfiguration();
    restartDevice();
    assertDefaultRoutingConfiguration();

    downloadRoutingConfiguration();
    gBau->flushPendingPersistence();
    masterReset(0x02);
    assertDefaultRoutingConfiguration();
    restartDevice();
    assertDefaultRoutingConfiguration();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_type3_download_with_matching_mcb_loads_the_table);
    RUN_TEST(test_type3_download_with_wrong_crc_ends_in_error);
    RUN_TEST(test_unload_clears_the_table);
    RUN_TEST(test_error_code_reads_crc_error_after_a_mismatch);
    RUN_TEST(test_downloaded_table_survives_restart);
    RUN_TEST(test_master_reset_factory_reset_clears_the_filter_table);
    RUN_TEST(test_memory_write_outside_loading_is_refused);
    RUN_TEST(test_routing_configuration_survives_restart);
    RUN_TEST(test_factory_reset_restores_the_default_routing_configuration);
    return UNITY_END();
}
