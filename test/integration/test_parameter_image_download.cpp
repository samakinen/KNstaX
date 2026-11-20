// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_parameter_image_download.cpp
 * @brief The parameter image of a product, downloaded the way ETS does it
 *
 * A product built with makeCommissionedProduct<kParameters>() runs on the real
 * stack over a mock TP1 physical layer. The test plays ETS on a transport
 * connection: StartLoading on the application program, A_Memory_Write into
 * the code segment, LoadCompleted, A_Restart. It checks what the product
 * reads, when it is told, and what survives a restart
 * (docs/design/parameter-model.md §3.4).
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

// ---------------------------------------------------------------------------
// The product: one switch port, a device block and a block repeated twice
// ---------------------------------------------------------------------------

enum class Port : uint16_t { Switch = 0 };

enum class Mode : uint8_t { Auto = 0, Timed = 1, Manual = 2 };

inline constexpr auto kModeOptions =
    params::options<Mode>({{Mode::Auto, "Automatic"}, {Mode::Timed, "Timed"}, {Mode::Manual, "Manual"}});

enum class Dev : uint8_t { OperatingMode, DelayMs };
enum class Ch : uint8_t { Enabled, Level };

inline constexpr auto kDeviceBlock = params::parameterBlock<Dev>(
    {.key = "device", .name = "Device"},
    params::param<Dev::OperatingMode, Mode>({.name = "Mode", .def = Mode::Auto, .options = kModeOptions}),
    params::param<Dev::DelayMs, uint16_t>({.name = "Delay", .def = 500, .min = 100, .max = 5000, .unit = "ms"}));

inline constexpr auto kChannelBlock = params::parameterBlock<Ch>(
    {.key = "channel", .name = "Channel {n}"},
    params::param<Ch::Enabled, bool>({.name = "Enabled", .def = true}),
    params::param<Ch::Level, uint8_t>({.name = "Level", .def = 50, .min = 0, .max = 100, .unit = "%"}));

inline constexpr auto kParameters = params::parameterLayout(kDeviceBlock, params::repeat<2>(kChannelBlock));

// The same product after a firmware update that added a third channel.
inline constexpr auto kParametersV2 = params::parameterLayout(kDeviceBlock, params::repeat<3>(kChannelBlock));

static_assert(std::remove_cvref_t<decltype(kParameters)>::kSize == 7u);
static_assert(std::remove_cvref_t<decltype(kParameters)>::kFingerprint
              != std::remove_cvref_t<decltype(kParametersV2)>::kFingerprint);

template <const auto& Layout>
constexpr auto makeTestProduct()
{
    return makeCommissionedProduct<Layout>(
        makeEndpointDefinition<Port, semantics::SwitchCommand<Port::Switch, "switch", "Switch">>(
            ProductIdentity{
                .productKey = "param_image",
                .productDisplayName = "Parameter Image Test",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::TP1,
                .applicationNumber = 78,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 15,
            },
            PersistencePolicy{
                .namespacePrefix = "param_image",
                .schemaVersion = 1,
                .persistKnxState = true,
            }));
}

inline constexpr auto kProduct = makeTestProduct<kParameters>();
inline constexpr auto kProductV2 = makeTestProduct<kParametersV2>();

// What ETS downloads: Manual, 1200 ms, channel 1 off at 10 %, channel 2 on at 100 %.
constexpr std::array<uint8_t, 7> kDownloaded{0x02, 0x04, 0xB0, 0x00, 0x0A, 0x01, 0x64};

// ---------------------------------------------------------------------------
// The bus and ETS
// ---------------------------------------------------------------------------

constexpr IndividualAddress kOwnAddress(1, 1, 7);
constexpr IndividualAddress kEtsAddress(0, 0, 1);
constexpr uint8_t kApplicationProgramIndex = 4;
constexpr uint16_t kFullMask = 0x3FFu;
constexpr uint16_t kGroupMask = 0x3C0u;

class RecordingPlatform final : public knx::platform::LinuxPlatform {
public:
    void restart() override { restartRequested = true; }
    bool restartRequested{false};
};

std::unique_ptr<RecordingPlatform> gPlatform;
MockPhysicalLayer* gPhysical = nullptr;
std::function<void()> gLoop;
int gChanged = 0;

struct SentFrame {
    protocol::TPCI tpci{};
    uint8_t sequenceNumber{0};
    uint16_t apci{0};
    std::vector<uint8_t> payload;
};

void clearPersistence()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("param_image"), ec);
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

    // MAX_FRAME_SIZE is the TPDU capacity; the encoded frame adds the header
    // (7 octets at most, extended frame) and the checksum.
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

/// One connected request from ETS; the device's numbered answers are
/// acknowledged the way ETS does.
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

/// An ETS session on a transport connection.
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

    /// PID_LOAD_STATE_CONTROL (5) of the application program.
    void loadControl(uint8_t event)
    {
        const auto sent = request(nextSeq(), application::APCIService::PropertyValueWrite, 0,
                                  {kApplicationProgramIndex, 0x05, 0x10, 0x01, event});
        TEST_ASSERT_NOT_NULL(findResponse(
            sent, kFullMask, static_cast<uint16_t>(application::APCIService::PropertyValueResponse)));
    }

    /// In pieces that fit the device's APDU, as ETS sends them: the APDU
    /// carries the APCI/count octet and the address besides the data.
    static constexpr size_t kChunk = config::MAX_APDU_LENGTH - 3u;

    void memoryWrite(uint16_t address, std::span<const uint8_t> data)
    {
        for (size_t at = 0; at < data.size(); at += kChunk) {
            const auto piece = data.subspan(at, std::min(kChunk, data.size() - at));
            const auto pieceAddress = static_cast<uint16_t>(address + at);
            std::vector<uint8_t> payload{static_cast<uint8_t>(pieceAddress >> 8),
                                         static_cast<uint8_t>(pieceAddress & 0xFFu)};
            payload.insert(payload.end(), piece.begin(), piece.end());
            (void)request(nextSeq(), application::APCIService::MemoryWrite, static_cast<uint8_t>(piece.size()), payload);
        }
    }

    /// The octets the device answers with; empty when it refuses any piece.
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

    /// A_Restart: ends every download, and makes the device store its state.
    void restart() { (void)request(nextSeq(), application::APCIService::Restart, 0); }

private:
    /// The transport layer's sequence number counts modulo 16.
    uint8_t nextSeq()
    {
        const uint8_t seq = _seq;
        _seq = static_cast<uint8_t>((_seq + 1u) & 0x0Fu);
        return seq;
    }

    uint8_t _seq{0};
};

constexpr uint16_t kImageBase = static_cast<uint16_t>(objects::tableseg::kApplicationCodeBase);

/// A whole parameter download, ended with the restart request.
void downloadLikeEts(std::span<const uint8_t> image, bool complete = true)
{
    EtsSession ets;
    ets.loadControl(objects::loadstate::kEventStartLoading);
    ets.memoryWrite(kImageBase, image);
    if (complete) {
        ets.loadControl(objects::loadstate::kEventLoadCompleted);
    }
    ets.restart();
}

/// Start the product on a fresh platform and bus, as after a power cycle.
template <const auto& Product>
auto startProduct()
{
    gPlatform = std::make_unique<RecordingPlatform>();
    auto physical = std::make_unique<MockPhysicalLayer>();
    gPhysical = physical.get();
    auto stackPort = createTp1TestStackPort(*gPlatform, std::move(physical));

    CommissionedProductOptions options{};
    options.standaloneDemoIndividualAddress = kOwnAddress;

    auto started = startCommissionedProduct(
        *gPlatform, Product,
        makeCommissionedBindings(Product)
            .template onCommand<Port::Switch>([](bool) {})
            .onParametersChanged([](auto /*parameters*/) { ++gChanged; }),
        std::move(stackPort), std::move(options));
    TEST_ASSERT_TRUE(started.isOk());
    auto product = std::move(started.value());
    gLoop = [raw = product.get()] { raw->loop(); };
    (void)pump(20);
    return product;
}

template <typename ProductT>
void stopProduct(ProductT& product)
{
    gLoop = [] {};
    product.reset();
    gPhysical = nullptr;
    gPlatform.reset();
}

} // namespace

void setUp(void)
{
    clearPersistence();
    gChanged = 0;
}

void tearDown(void)
{
    gLoop = [] {};
    gPhysical = nullptr;
    gPlatform.reset();
}

// A device that was never downloaded reads the declared defaults.
void test_a_new_device_reads_the_defaults(void)
{
    auto product = startProduct<kProduct>();
    const auto parameters = product->parameters();

    TEST_ASSERT_TRUE(parameters.block<Dev>().get<Dev::OperatingMode>() == Mode::Auto);
    TEST_ASSERT_EQUAL_UINT16(500u, parameters.block<Dev>().get<Dev::DelayMs>());
    for (size_t channel = 0; channel < 2u; ++channel) {
        TEST_ASSERT_TRUE(parameters.block<Ch>(channel).get<Ch::Enabled>());
        TEST_ASSERT_EQUAL_UINT8(50u, parameters.block<Ch>(channel).get<Ch::Level>());
    }
    TEST_ASSERT_TRUE(product->parametersLoaded());
    TEST_ASSERT_EQUAL_INT(0, gChanged);  // not called at start

    stopProduct(product);
}

// The download: ETS writes into the image, reads it back, and LoadCompleted
// tells the product once.
void test_an_ets_download_reaches_the_product(void)
{
    auto product = startProduct<kProduct>();
    {
        EtsSession ets;
        ets.loadControl(objects::loadstate::kEventStartLoading);
        TEST_ASSERT_FALSE(product->parametersLoaded());  // a download is running

        ets.memoryWrite(kImageBase, kDownloaded);
        TEST_ASSERT_EQUAL_INT(0, gChanged);  // not before LoadCompleted

        const auto readBack = ets.memoryRead(kImageBase, static_cast<uint8_t>(kDownloaded.size()));
        TEST_ASSERT_EQUAL_UINT32(kDownloaded.size(), readBack.size());
        TEST_ASSERT_TRUE(std::equal(kDownloaded.begin(), kDownloaded.end(), readBack.begin()));

        // The image ends where the layout ends: one octet more is refused.
        TEST_ASSERT_TRUE(ets.memoryRead(kImageBase, static_cast<uint8_t>(kDownloaded.size() + 1u)).empty());

        ets.loadControl(objects::loadstate::kEventLoadCompleted);
    }

    TEST_ASSERT_EQUAL_INT(1, gChanged);
    TEST_ASSERT_TRUE(product->parametersLoaded());
    const auto parameters = product->parameters();
    TEST_ASSERT_TRUE(parameters.block<Dev>().get<Dev::OperatingMode>() == Mode::Manual);
    TEST_ASSERT_EQUAL_UINT16(1200u, parameters.block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_FALSE(parameters.block<Ch>(0).get<Ch::Enabled>());
    TEST_ASSERT_EQUAL_UINT8(10u, parameters.block<Ch>(0).get<Ch::Level>());
    TEST_ASSERT_TRUE(parameters.block<Ch>(1).get<Ch::Enabled>());
    TEST_ASSERT_EQUAL_UINT8(100u, parameters.block<Ch>(1).get<Ch::Level>());

    stopProduct(product);
}

// The downloaded image is stored and is back after a restart.
void test_a_download_survives_a_restart(void)
{
    auto product = startProduct<kProduct>();
    downloadLikeEts(kDownloaded);
    TEST_ASSERT_TRUE(gPlatform->restartRequested);
    stopProduct(product);

    gChanged = 0;
    auto restarted = startProduct<kProduct>();
    TEST_ASSERT_TRUE(restarted->parametersLoaded());
    TEST_ASSERT_EQUAL_UINT16(1200u, restarted->parameters().block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_EQUAL_UINT8(100u, restarted->parameters().block<Ch>(1).get<Ch::Level>());
    TEST_ASSERT_EQUAL_INT(0, gChanged);

    stopProduct(restarted);
}

// A value outside its bounds: the image is refused as a whole.
void test_an_image_outside_its_bounds_is_refused(void)
{
    auto product = startProduct<kProduct>();
    auto image = kDownloaded;
    image[6] = 200;  // channel 2 level, the maximum is 100
    downloadLikeEts(image);

    TEST_ASSERT_EQUAL_INT(0, gChanged);
    TEST_ASSERT_FALSE(product->parametersLoaded());
    TEST_ASSERT_EQUAL_UINT16(500u, product->parameters().block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_EQUAL_UINT8(50u, product->parameters().block<Ch>(1).get<Ch::Level>());
    stopProduct(product);

    // And it stays refused: the restart does not turn the defaults into a
    // loaded application.
    auto restarted = startProduct<kProduct>();
    TEST_ASSERT_FALSE(restarted->parametersLoaded());
    TEST_ASSERT_EQUAL_UINT8(50u, restarted->parameters().block<Ch>(1).get<Ch::Level>());
    stopProduct(restarted);
}

// A download that never reaches LoadCompleted: after the restart the last
// completed download is in force again.
void test_an_interrupted_download_leaves_the_last_completed_one(void)
{
    auto product = startProduct<kProduct>();
    downloadLikeEts(kDownloaded);
    stopProduct(product);

    auto second = startProduct<kProduct>();
    const std::array<uint8_t, 7> other{0x01, 0x00, 0xC8, 0x01, 0x01, 0x01, 0x02};
    downloadLikeEts(other, /*complete=*/false);
    TEST_ASSERT_FALSE(second->parametersLoaded());
    TEST_ASSERT_EQUAL_INT(1, gChanged);  // only the first download was announced
    stopProduct(second);

    auto restarted = startProduct<kProduct>();
    TEST_ASSERT_TRUE(restarted->parametersLoaded());
    TEST_ASSERT_TRUE(restarted->parameters().block<Dev>().get<Dev::OperatingMode>() == Mode::Manual);
    TEST_ASSERT_EQUAL_UINT16(1200u, restarted->parameters().block<Dev>().get<Dev::DelayMs>());
    stopProduct(restarted);
}

// New firmware with another layout does not read the old image: defaults, and
// the application program reports that it needs a download.
void test_an_image_of_another_layout_is_not_taken_into_use(void)
{
    auto product = startProduct<kProduct>();
    downloadLikeEts(kDownloaded);
    stopProduct(product);

    auto updated = startProduct<kProductV2>();
    TEST_ASSERT_FALSE(updated->parametersLoaded());
    TEST_ASSERT_TRUE(updated->parameters().block<Dev>().get<Dev::OperatingMode>() == Mode::Auto);
    TEST_ASSERT_EQUAL_UINT16(500u, updated->parameters().block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_EQUAL_UINT8(50u, updated->parameters().block<Ch>(2).get<Ch::Level>());

    // A download for the new layout puts it right.
    const std::array<uint8_t, 9> image{0x01, 0x03, 0xE8, 0x01, 0x14, 0x01, 0x1E, 0x00, 0x28};
    downloadLikeEts(image);
    TEST_ASSERT_TRUE(updated->parametersLoaded());
    TEST_ASSERT_EQUAL_UINT16(1000u, updated->parameters().block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_EQUAL_UINT8(40u, updated->parameters().block<Ch>(2).get<Ch::Level>());
    stopProduct(updated);
}

// The device changes a parameter itself: checked, announced and stored.
void test_the_product_sets_a_parameter(void)
{
    auto product = startProduct<kProduct>();
    // The coalesced save runs on the product's clock.
    product->setTimeSource([] {
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    });

    TEST_ASSERT_TRUE(product->setParameter<Ch::Level>(1, uint8_t{80}).isOk());
    TEST_ASSERT_EQUAL_INT(1, gChanged);
    TEST_ASSERT_EQUAL_UINT8(80u, product->parameters().block<Ch>(1).get<Ch::Level>());
    TEST_ASSERT_EQUAL_UINT8(50u, product->parameters().block<Ch>(0).get<Ch::Level>());

    TEST_ASSERT_TRUE(product->setParameter<Dev::DelayMs>(uint16_t{2000}).isOk());
    TEST_ASSERT_TRUE(product->setParameter<Dev::OperatingMode>(Mode::Timed).isOk());
    TEST_ASSERT_EQUAL_INT(3, gChanged);

    // Outside the declared bounds, and an instance the block does not have.
    TEST_ASSERT_TRUE(product->setParameter<Ch::Level>(1, uint8_t{150}).isError());
    TEST_ASSERT_TRUE(product->setParameter<Dev::DelayMs>(uint16_t{50}).isError());
    TEST_ASSERT_TRUE(product->setParameter<Ch::Level>(2, uint8_t{10}).isError());
    TEST_ASSERT_EQUAL_INT(3, gChanged);
    TEST_ASSERT_EQUAL_UINT8(80u, product->parameters().block<Ch>(1).get<Ch::Level>());
    TEST_ASSERT_EQUAL_UINT16(2000u, product->parameters().block<Dev>().get<Dev::DelayMs>());

    (void)pump(700);  // the coalesced save
    stopProduct(product);

    auto restarted = startProduct<kProduct>();
    TEST_ASSERT_EQUAL_UINT8(80u, restarted->parameters().block<Ch>(1).get<Ch::Level>());
    TEST_ASSERT_EQUAL_UINT16(2000u, restarted->parameters().block<Dev>().get<Dev::DelayMs>());
    TEST_ASSERT_TRUE(restarted->parameters().block<Dev>().get<Dev::OperatingMode>() == Mode::Timed);
    stopProduct(restarted);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_new_device_reads_the_defaults);
    RUN_TEST(test_an_ets_download_reaches_the_product);
    RUN_TEST(test_a_download_survives_a_restart);
    RUN_TEST(test_an_image_outside_its_bounds_is_refused);
    RUN_TEST(test_an_interrupted_download_leaves_the_last_completed_one);
    RUN_TEST(test_an_image_of_another_layout_is_not_taken_into_use);
    RUN_TEST(test_the_product_sets_a_parameter);
    return UNITY_END();
}
