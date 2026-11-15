// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_flash_log_persistence.cpp
 * @brief The interface object manager over FlashLogPersistence on the flash
 *        simulator: a download survives a restart, and a power cut at any step
 *        of saving a second download boots with the first or the second, never
 *        a mixture
 */

#include "unity.h"

#include "knx/objects/address_table_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/objects/flash_hal_virtual.hpp"
#include "knx/objects/flash_log.hpp"
#include "knx/objects/flash_log_persistence.hpp"
#include "knx/objects/interface_object_manager.hpp"

#include <cstdio>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::objects;
using util::ErrorCode;

// The Unity stand-in has no _MESSAGE assertions.
#define CHECK(condition, text)       \
    do {                             \
        if (!(condition)) {          \
            TEST_FAIL_MESSAGE(text); \
        }                            \
    } while (0)

namespace {

FlashLog* g_log = nullptr;

std::unique_ptr<ObjectPersistence> makeFlashPersistence() { return std::make_unique<FlashLogPersistence>(*g_log); }

struct Download {
    uint8_t subnet;
    uint8_t device;
    std::vector<uint8_t> applicationId;  // 2 octets
    std::vector<uint8_t> addressTable;   // entries from index 1, 2 octets each

    bool operator==(const Download&) const = default;
};

const Download kFirst{0x11, 0x04, {0xBE, 0xEF}, {0x08, 0x01, 0x08, 0x02}};
const Download kSecond{0x22, 0x07, {0xCA, 0xFE}, {0x09, 0x01, 0x09, 0x02, 0x09, 0x03}};

template <typename PropertyT>
application::PropertyID pid(PropertyT p)
{
    return static_cast<application::PropertyID>(p);
}

void write(InterfaceObjectManager& mgr, const Download& d)
{
    const InterfaceObjectInstance one(1);
    const std::vector<uint8_t> progMode{0x01};
    TEST_ASSERT_TRUE(mgr.writeProperty(InterfaceObjectType::device(), one, pid(DeviceProperty::ProgMode), 1, progMode).isOk());
    TEST_ASSERT_TRUE(mgr.writeProperty(InterfaceObjectType::device(), one, pid(DeviceProperty::SubnetAddress), 1,
                                       std::vector<uint8_t>{d.subnet}).isOk());
    TEST_ASSERT_TRUE(mgr.writeProperty(InterfaceObjectType::device(), one, pid(DeviceProperty::DeviceAddress), 1,
                                       std::vector<uint8_t>{d.device}).isOk());
    TEST_ASSERT_TRUE(mgr.writeProperty(InterfaceObjectType::applicationProgram(), one,
                                       pid(AppProgramProperty::ApplicationId), 1, d.applicationId).isOk());
    TEST_ASSERT_TRUE(mgr.writeProperty(InterfaceObjectType::addressTable(), one, pid(AddressTableProperty::TableData), 1,
                                       d.addressTable).isOk());
}

Download read(InterfaceObjectManager& mgr)
{
    const InterfaceObjectInstance one(1);
    Download d{};
    application::PropertyServiceDataBuffer out;
    TEST_ASSERT_TRUE(mgr.readProperty(InterfaceObjectType::device(), one, pid(DeviceProperty::SubnetAddress), 1, 1, out).isOk());
    d.subnet = out[0];
    TEST_ASSERT_TRUE(mgr.readProperty(InterfaceObjectType::device(), one, pid(DeviceProperty::DeviceAddress), 1, 1, out).isOk());
    d.device = out[0];
    TEST_ASSERT_TRUE(mgr.readProperty(InterfaceObjectType::applicationProgram(), one,
                                      pid(AppProgramProperty::ApplicationId), 1, 1, out).isOk());
    d.applicationId.assign(out.begin(), out.end());
    // Element 0 is the entry count.
    TEST_ASSERT_TRUE(mgr.readProperty(InterfaceObjectType::addressTable(), one, pid(AddressTableProperty::TableData), 0, 1, out).isOk());
    const uint16_t count = static_cast<uint16_t>((out[0] << 8) | out[1]);
    for (uint16_t i = 1; i <= count; ++i) {
        TEST_ASSERT_TRUE(mgr.readProperty(InterfaceObjectType::addressTable(), one, pid(AddressTableProperty::TableData), i, 1, out).isOk());
        d.addressTable.insert(d.addressTable.end(), out.begin(), out.end());
    }
    return d;
}

void drain(FlashLog& log)
{
    for (int i = 0; i < 64 && log.servicePending(); ++i) {
        TEST_ASSERT_TRUE(log.service().isOk());
    }
}

/// One boot: mount, start the manager on the log, load what is stored.
std::unique_ptr<InterfaceObjectManager> boot(FlashLog& log)
{
    g_log = &log;
    auto mgr = std::make_unique<InterfaceObjectManager>();
    TEST_ASSERT_TRUE(mgr->init(true).isOk());
    (void)mgr->loadFromPersistence();
    return mgr;
}

}  // namespace

void setUp(void) { setPersistenceFactory(&makeFlashPersistence); }
void tearDown(void)
{
    setPersistenceFactory(nullptr);
    g_log = nullptr;
}

void test_download_survives_restart(void)
{
    FlashHalVirtual flash({0x0801E000u, 1024u, 8u, 2u});  // the H8i8o region: two banks of 4 KiB
    {
        FlashLog log(flash.hal());
        auto mgr = boot(log);
        write(*mgr, kFirst);
        TEST_ASSERT_TRUE(mgr->saveToPersistence().isOk());
        std::printf("one save: %lu of %lu bytes used\n",
                    static_cast<unsigned long>(log.statistics().bankBytes - log.statistics().freeBytes),
                    static_cast<unsigned long>(log.statistics().bankBytes));
    }
    FlashLog log(flash.hal());
    auto mgr = boot(log);
    TEST_ASSERT_TRUE(kFirst == read(*mgr));
    TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
}

void test_string_keys_are_not_supported(void)
{
    FlashHalVirtual flash({0, 256u, 2u, 2u});
    FlashLog log(flash.hal());
    FlashLogPersistence p(log);
    TEST_ASSERT_TRUE(p.init("ignored").isOk());
    std::vector<uint8_t> out;
    TEST_ASSERT_EQUAL(ErrorCode::OperationNotSupported, p.save("k", out).error());
    TEST_ASSERT_EQUAL(ErrorCode::OperationNotSupported, p.load("k", out).error());
    TEST_ASSERT_TRUE(p.saveById(0x1234, std::vector<uint8_t>{1}).isOk());
    const auto keys = p.listKeys();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(keys.size()));
    TEST_ASSERT_EQUAL_STRING("d1234", keys[0].c_str());
}

namespace {

struct CutRun {
    bool cut = false;
    uint32_t compactions = 0;  // during the interrupted save
};

/// @p priorSaves downloads of kFirst, then kSecond saved with the power cut
/// after @p cutAfter steps; the next boot must find one of the two, whole.
CutRun saveSecondWithCut(const knx_flash_hal_geometry_t& geometry, int priorSaves, uint64_t cutAfter)
{
    FlashHalVirtual flash(geometry);
    CutRun run;
    {
        FlashLog log(flash.hal());
        auto mgr = boot(log);
        write(*mgr, kFirst);
        for (int i = 0; i < priorSaves; ++i) {
            TEST_ASSERT_TRUE(mgr->saveToPersistence().isOk());
            drain(log);
        }
        write(*mgr, kSecond);
        const uint32_t before = log.statistics().compactions;
        flash.cutPowerAfter(cutAfter);
        const auto saved = mgr->saveToPersistence();
        run.cut = flash.powerCut();
        run.compactions = log.statistics().compactions - before;
        CHECK(run.cut || saved.isOk(), "save failed without a power cut");
    }
    flash.powerOn();

    char message[96];
    std::snprintf(message, sizeof(message), "%d prior save(s), cut after %llu steps", priorSaves,
                  static_cast<unsigned long long>(cutAfter));
    FlashLog log(flash.hal());
    auto mgr = boot(log);
    const Download found = read(*mgr);
    CHECK(found == kFirst || found == kSecond, message);
    if (!run.cut) {
        CHECK(found == kSecond, message);
    }
    // And the device can take the next download.
    drain(log);
    write(*mgr, kSecond);
    CHECK(mgr->saveToPersistence().isOk(), message);
    CHECK((0u) == (flash.contractViolations()), message);
    return run;
}

void cutEverywhere(const knx_flash_hal_geometry_t& geometry, int priorSaves, uint32_t& compactingRuns)
{
    uint64_t cutAfter = 0;
    for (;;) {
        const CutRun run = saveSecondWithCut(geometry, priorSaves, cutAfter);
        if (!run.cut) {
            break;
        }
        compactingRuns += run.compactions != 0u ? 1u : 0u;
        ++cutAfter;
    }
    TEST_ASSERT_TRUE(cutAfter > 50u);
}

}  // namespace

void test_power_cut_during_a_save_keeps_one_whole_download(void)
{
    uint32_t compactingRuns = 0;
    cutEverywhere({0x0801E000u, 1024u, 8u, 2u}, 1, compactingRuns);
    // Small banks, so that the interrupted save has to compact.
    for (int prior = 1; prior <= 3; ++prior) {
        cutEverywhere({0x08000000u, 256u, 6u, 2u}, prior, compactingRuns);
    }
    TEST_ASSERT_TRUE(compactingRuns > 0u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_download_survives_restart);
    RUN_TEST(test_string_keys_are_not_supported);
    RUN_TEST(test_power_cut_during_a_save_keeps_one_whole_download);
    return UNITY_END();
}
