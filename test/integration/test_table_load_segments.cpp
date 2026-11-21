// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_table_load_segments.cpp
 * @brief The address and association tables, loaded the way ETS does it
 *
 * A product runs on the real stack over a mock TP1 physical layer. The test
 * plays ETS on a transport connection: StartLoading on a table object,
 * A_Memory_Write into its load segment, a read-back, LoadCompleted. It checks
 * the load state and what a memory read of the segment answers.
 *
 * With KNX_SHARED_LOAD_SEGMENT (tp1-small, host-minimal) the three segments
 * share one staging buffer, and the shared-segment tests check what that
 * changes: a write outside a load is refused, a segment outside a load reads
 * from its table, and a load that loses the buffer to another table fails.
 *
 * CTest label: "integration"
 */

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/product/commissioned_product.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::product;
using namespace knx::test;

namespace {

enum class Port : uint16_t { Switch = 0 };

inline constexpr auto kProduct = makeCommissionedProduct(
    makeEndpointDefinition<Port, semantics::SwitchCommand<Port::Switch, "switch", "Switch">>(
        ProductIdentity{
            .productKey = "table_segments",
            .productDisplayName = "Table Segment Test",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = endpoint::Medium::TP1,
            .applicationNumber = 79,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 15,
        },
        PersistencePolicy{
            .namespacePrefix = "table_segments",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

constexpr IndividualAddress kOwnAddress(1, 1, 7);
constexpr IndividualAddress kEtsAddress(0, 0, 1);
constexpr uint16_t kFullMask = 0x3FFu;
constexpr uint16_t kGroupMask = 0x3C0u;

// Interface object indices of the tables, as ETS addresses them.
constexpr uint8_t kAddressTableIndex = 1;
constexpr uint8_t kAssociationTableIndex = 2;
constexpr uint8_t kGroupObjectTableIndex = 3;

constexpr auto kAddressBase = static_cast<uint16_t>(objects::tableseg::kGroupAddressTableBase);
constexpr auto kAssociationBase = static_cast<uint16_t>(objects::tableseg::kAssociationTableBase);
constexpr auto kGroupObjectBase = static_cast<uint16_t>(objects::tableseg::kGroupObjectTableBase);

// Two group addresses, 1/0/1 and 1/0/2, and an association of each with object 0.
constexpr std::array<uint8_t, 6> kAddresses{0x00, 0x02, 0x08, 0x01, 0x08, 0x02};
constexpr std::array<uint8_t, 10> kAssociations{0x00, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00};

std::unique_ptr<knx::platform::LinuxPlatform> gPlatform;
MockPhysicalLayer* gPhysical = nullptr;
std::function<void()> gLoop;

struct SentFrame {
    protocol::TPCI tpci{};
    uint8_t sequenceNumber{0};
    uint16_t apci{0};
    std::vector<uint8_t> payload;
};

void clearPersistence()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("table_segments"), ec);
}

void inject(std::span<const uint8_t> tpdu)
{
    datalink::LDataFrame frame;
    frame.source = kEtsAddress;
    frame.destination = GroupAddress(kOwnAddress.raw);
    frame.destinationType = AddressType::Individual;
    frame.priority = Priority::System;
    frame.hopCount = 6;
    frame.tpdu.assign(tpdu);
    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE + 8u> buffer{};
    auto encoded = datalink::FrameCodec::encodeFrame(frame, buffer);
    TEST_ASSERT_TRUE(encoded.isOk());
    gPhysical->injectFrame(std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value()));
}

std::vector<SentFrame> pump(int budgetMs = 120)
{
    std::vector<SentFrame> sent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        gLoop();
        std::vector<uint8_t> wire;
        while (gPhysical->getSentFrame(wire)) {
            datalink::LDataFrame decoded;
            if (datalink::FrameCodec::decodeFrame(wire, decoded).isError() || decoded.tpdu.empty()) {
                continue;
            }
            const auto header = protocol::unpackTpduHeader(decoded.tpdu[0],
                                                           decoded.tpdu.size() > 1u ? decoded.tpdu[1] : 0u);
            sent.push_back(SentFrame{header.tpci.type(), header.tpci.seqNum(), header.apci.raw,
                                     std::vector<uint8_t>(
                                         decoded.tpdu.begin() + (decoded.tpdu.size() > 2u ? 2 : decoded.tpdu.size()),
                                         decoded.tpdu.end())});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return sent;
}

const SentFrame* findResponse(const std::vector<SentFrame>& sent, uint16_t apciMask, uint16_t apci)
{
    for (const auto& frame : sent) {
        if ((frame.tpci == protocol::TPCI::UnnumberedData || frame.tpci == protocol::TPCI::NumberedData)
            && (frame.apci & apciMask) == apci) {
            return &frame;
        }
    }
    return nullptr;
}

std::vector<SentFrame> request(uint8_t seq, application::APCIService service, uint8_t data6 = 0,
                               std::vector<uint8_t> payload = {})
{
    const auto tpdu = protocol::buildTpdu(protocol::TPCIField::create(protocol::TPCI::NumberedData, seq),
                                          application::APCIField::create(service, data6),
                                          std::span<const uint8_t>(payload));
    inject(std::vector<uint8_t>(tpdu.begin(), tpdu.end()));
    auto sent = pump();
    for (const auto& frame : sent) {
        if (frame.tpci == protocol::TPCI::NumberedData) {
            const auto ack = protocol::buildTpdu(protocol::TPCIField::ack(frame.sequenceNumber),
                                                 application::APCIField(0), std::span<const uint8_t>{});
            inject(std::vector<uint8_t>(ack.begin(), ack.end()));
            (void)pump(30);
        }
    }
    return sent;
}

class EtsSession {
public:
    EtsSession()
    {
        inject(std::vector<uint8_t>{0x80});  // T_Connect
        (void)pump(60);
    }

    ~EtsSession()
    {
        inject(std::vector<uint8_t>{0x81});  // T_Disconnect
        (void)pump(30);
    }

    /// PID_LOAD_STATE_CONTROL (5) of the object at @p objectIndex.
    void loadControl(uint8_t objectIndex, uint8_t event)
    {
        const auto sent = request(nextSeq(), application::APCIService::PropertyValueWrite, 0,
                                  {objectIndex, 0x05, 0x10, 0x01, event});
        TEST_ASSERT_NOT_NULL(findResponse(
            sent, kFullMask, static_cast<uint16_t>(application::APCIService::PropertyValueResponse)));
    }

    /// The load state the object reports (PID_LOAD_STATE_CONTROL read).
    uint8_t loadState(uint8_t objectIndex)
    {
        const auto sent =
            request(nextSeq(), application::APCIService::PropertyValueRead, 0, {objectIndex, 0x05, 0x10, 0x01});
        const auto* answer =
            findResponse(sent, kFullMask, static_cast<uint16_t>(application::APCIService::PropertyValueResponse));
        TEST_ASSERT_NOT_NULL(answer);
        TEST_ASSERT_TRUE(answer->payload.size() >= 5u);
        return answer->payload[4];
    }

    static constexpr size_t kChunk = config::MAX_APDU_LENGTH - 3u;

    void memoryWrite(uint16_t address, std::span<const uint8_t> data)
    {
        for (size_t at = 0; at < data.size(); at += kChunk) {
            const auto piece = data.subspan(at, std::min(kChunk, data.size() - at));
            const auto pieceAddress = static_cast<uint16_t>(address + at);
            std::vector<uint8_t> payload{static_cast<uint8_t>(pieceAddress >> 8),
                                         static_cast<uint8_t>(pieceAddress & 0xFFu)};
            payload.insert(payload.end(), piece.begin(), piece.end());
            (void)request(nextSeq(), application::APCIService::MemoryWrite, static_cast<uint8_t>(piece.size()),
                          payload);
        }
    }

    std::vector<uint8_t> memoryRead(uint16_t address, uint8_t count)
    {
        std::vector<uint8_t> octets;
        for (size_t at = 0; at < count; at += kChunk) {
            const auto pieceCount = static_cast<uint8_t>(std::min<size_t>(kChunk, count - at));
            const auto pieceAddress = static_cast<uint16_t>(address + at);
            const auto sent = request(nextSeq(), application::APCIService::MemoryRead, pieceCount,
                                      {static_cast<uint8_t>(pieceAddress >> 8),
                                       static_cast<uint8_t>(pieceAddress & 0xFFu)});
            const auto* answer =
                findResponse(sent, kGroupMask, static_cast<uint16_t>(application::APCIService::MemoryResponse));
            if (answer == nullptr || answer->payload.size() < 2u + pieceCount) {
                return {};
            }
            octets.insert(octets.end(), answer->payload.begin() + 2, answer->payload.end());
        }
        return octets;
    }

    /// StartLoading, the segment, a read-back of it, LoadCompleted.
    void loadTable(uint8_t objectIndex, uint16_t base, std::span<const uint8_t> content)
    {
        loadControl(objectIndex, objects::loadstate::kEventStartLoading);
        memoryWrite(base, content);
        const auto readBack = memoryRead(base, static_cast<uint8_t>(content.size()));
        TEST_ASSERT_EQUAL_UINT(content.size(), readBack.size());
        TEST_ASSERT_EQUAL_MEMORY(content.data(), readBack.data(), content.size());
        loadControl(objectIndex, objects::loadstate::kEventLoadCompleted);
    }

private:
    uint8_t nextSeq()
    {
        const uint8_t seq = _seq;
        _seq = static_cast<uint8_t>((_seq + 1u) & 0x0Fu);
        return seq;
    }

    uint8_t _seq{0};
};

auto startProduct()
{
    gPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto physical = std::make_unique<MockPhysicalLayer>();
    gPhysical = physical.get();
    auto stackPort = createTp1TestStackPort(*gPlatform, std::move(physical));

    CommissionedProductOptions options{};
    options.standaloneDemoIndividualAddress = kOwnAddress;

    auto started = startCommissionedProduct(*gPlatform, kProduct,
                                            makeCommissionedBindings(kProduct).onCommand<Port::Switch>([](bool) {}),
                                            std::move(stackPort), std::move(options));
    TEST_ASSERT_TRUE(started.isOk());
    auto product = std::move(started.value());
    gLoop = [raw = product.get()] { raw->loop(); };
    (void)pump(20);
    return product;
}

}  // namespace

void setUp(void) { clearPersistence(); }

void tearDown(void)
{
    gLoop = [] {};
    gPhysical = nullptr;
    gPlatform.reset();
}

// Both tables load, read back what was written while loading, and end Loaded.
void test_the_tables_load_like_ets_loads_them(void)
{
    auto product = startProduct();
    {
        EtsSession ets;
        ets.loadTable(kAddressTableIndex, kAddressBase, kAddresses);
        ets.loadTable(kAssociationTableIndex, kAssociationBase, kAssociations);
        TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAddressTableIndex));
        TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAssociationTableIndex));
    }
    product.reset();
}

#if KNX_SHARED_LOAD_SEGMENT

// After the load the buffer is free; the segments read as their tables.
void test_a_loaded_segment_reads_from_its_table(void)
{
    auto product = startProduct();
    EtsSession ets;
    ets.loadTable(kAddressTableIndex, kAddressBase, kAddresses);
    ets.loadTable(kAssociationTableIndex, kAssociationBase, kAssociations);

    const auto addresses = ets.memoryRead(kAddressBase, static_cast<uint8_t>(kAddresses.size()));
    TEST_ASSERT_EQUAL_UINT(kAddresses.size(), addresses.size());
    TEST_ASSERT_EQUAL_MEMORY(kAddresses.data(), addresses.data(), kAddresses.size());
    const auto associations = ets.memoryRead(kAssociationBase, static_cast<uint8_t>(kAssociations.size()));
    TEST_ASSERT_EQUAL_UINT(kAssociations.size(), associations.size());
    TEST_ASSERT_EQUAL_MEMORY(kAssociations.data(), associations.data(), kAssociations.size());
    // The group object table has no table behind it: zeros.
    const auto groupObjects = ets.memoryRead(kGroupObjectBase, 4u);
    TEST_ASSERT_EQUAL_UINT(4u, groupObjects.size());
    const std::array<uint8_t, 4> zeros{};
    TEST_ASSERT_EQUAL_MEMORY(zeros.data(), groupObjects.data(), zeros.size());
}

// A write into a segment whose table is not loading is refused: the segment
// still reads as the table.
void test_a_write_outside_a_load_is_refused(void)
{
    auto product = startProduct();
    EtsSession ets;
    ets.loadTable(kAddressTableIndex, kAddressBase, kAddresses);

    const std::array<uint8_t, 4> stray{0x00, 0x01, 0x09, 0x09};
    ets.memoryWrite(kAddressBase, stray);
    const auto addresses = ets.memoryRead(kAddressBase, static_cast<uint8_t>(kAddresses.size()));
    TEST_ASSERT_EQUAL_MEMORY(kAddresses.data(), addresses.data(), kAddresses.size());
    TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAddressTableIndex));
}

// Two tables loading at once: the second StartLoading takes the buffer, the
// second load completes, and the first fails rather than loading nothing.
void test_a_load_that_loses_the_buffer_fails(void)
{
    auto product = startProduct();
    EtsSession ets;
    ets.loadControl(kAddressTableIndex, objects::loadstate::kEventStartLoading);
    ets.memoryWrite(kAddressBase, kAddresses);
    ets.loadTable(kAssociationTableIndex, kAssociationBase, kAssociations);
    ets.loadControl(kAddressTableIndex, objects::loadstate::kEventLoadCompleted);

    TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kError, ets.loadState(kAddressTableIndex));
    TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAssociationTableIndex));
}

// An abandoned load does not block the next one: StartLoading takes the buffer.
void test_an_abandoned_load_does_not_block_the_next(void)
{
    auto product = startProduct();
    EtsSession ets;
    ets.loadControl(kAssociationTableIndex, objects::loadstate::kEventStartLoading);
    ets.memoryWrite(kAssociationBase, kAssociations);
    // ETS gives up here and starts again with the address table.
    ets.loadTable(kAddressTableIndex, kAddressBase, kAddresses);
    ets.loadTable(kAssociationTableIndex, kAssociationBase, kAssociations);
    TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAddressTableIndex));
    TEST_ASSERT_EQUAL_UINT8(objects::loadstate::kLoaded, ets.loadState(kAssociationTableIndex));
}

#endif

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_tables_load_like_ets_loads_them);
#if KNX_SHARED_LOAD_SEGMENT
    RUN_TEST(test_a_loaded_segment_reads_from_its_table);
    RUN_TEST(test_a_write_outside_a_load_is_refused);
    RUN_TEST(test_a_load_that_loses_the_buffer_fails);
    RUN_TEST(test_an_abandoned_load_does_not_block_the_next);
#endif
    return UNITY_END();
}
