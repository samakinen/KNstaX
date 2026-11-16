// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_flash_log.cpp
 * @brief FlashLog on the flash simulator: format, records, compaction, erase
 *        scheduling, and a power cut at every program and erase step
 */

#include "unity.h"

#include "knx/objects/flash_hal_virtual.hpp"
#include "knx/objects/flash_log.hpp"

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
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

// Small banks (2 pages of 256 B) so a handful of records fills one.
knx_flash_hal_geometry_t geometry(uint32_t writeUnit, uint32_t pageSize = 256u, uint32_t pageCount = 4u)
{
    return knx_flash_hal_geometry_t{0x0801F000u, pageSize, pageCount, writeUnit};
}

std::vector<uint8_t> pattern(uint16_t id, uint8_t generation, size_t length)
{
    std::vector<uint8_t> v(length);
    for (size_t i = 0; i < length; ++i) {
        v[i] = static_cast<uint8_t>(id * 31u + generation * 7u + i);
    }
    return v;
}

std::optional<std::vector<uint8_t>> loadOpt(FlashLog& log, uint16_t id)
{
    std::vector<uint8_t> out;
    const auto r = log.load(id, out);
    if (r.error() == ErrorCode::NotFound) {
        return std::nullopt;
    }
    CHECK(r.isOk(), "load failed");
    return out;
}

void drainService(FlashLog& log)
{
    for (int i = 0; i < 64 && log.servicePending(); ++i) {
        TEST_ASSERT_TRUE(log.service().isOk());
    }
    TEST_ASSERT_FALSE(log.servicePending());
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_invalid_geometry_is_refused(void)
{
    FlashHalVirtual odd(geometry(2u, 256u, 3u));
    FlashLog a(odd.hal());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, a.mount().error());

    FlashHalVirtual unit(geometry(3u));
    FlashLog b(unit.hal());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, b.mount().error());

    FlashLog unmounted(odd.hal());
    TEST_ASSERT_EQUAL(ErrorCode::NotInitialized, unmounted.save(1, pattern(1, 0, 4)).error());
}

void test_blank_region_is_formatted_once(void)
{
    FlashHalVirtual flash(geometry(2u));
    {
        FlashLog log(flash.hal());
        TEST_ASSERT_TRUE(log.mount().isOk());
        TEST_ASSERT_EQUAL_UINT32(1u, log.statistics().formats);
        TEST_ASSERT_EQUAL_UINT32(1u, log.statistics().sequence);
        TEST_ASSERT_FALSE(loadOpt(log, 7).has_value());
        TEST_ASSERT_FALSE(log.servicePending());
    }
    FlashLog again(flash.hal());
    TEST_ASSERT_TRUE(again.mount().isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, again.statistics().formats);
}

void test_latest_record_wins_and_survives_remount(void)
{
    for (const uint32_t unit : {1u, 2u, 8u, 32u}) {
        FlashHalVirtual flash(geometry(unit));
        {
            FlashLog log(flash.hal());
            TEST_ASSERT_TRUE(log.mount().isOk());
            TEST_ASSERT_TRUE(log.save(0xFFFF, pattern(0xFFFF, 0, 2)).isOk());  // every id is usable
            TEST_ASSERT_TRUE(log.save(5, pattern(5, 0, 13)).isOk());
            TEST_ASSERT_TRUE(log.save(5, pattern(5, 1, 3)).isOk());
            TEST_ASSERT_TRUE(log.save(6, {}).isOk());
        }
        FlashLog log(flash.hal());
        TEST_ASSERT_TRUE(log.mount().isOk());
        TEST_ASSERT_TRUE(pattern(5, 1, 3) == *loadOpt(log, 5));
        TEST_ASSERT_TRUE(pattern(0xFFFF, 0, 2) == *loadOpt(log, 0xFFFF));
        const auto empty = loadOpt(log, 6);
        TEST_ASSERT_TRUE(empty.has_value() && empty->empty());
        TEST_ASSERT_TRUE(log.exists(6).value());
        TEST_ASSERT_FALSE(log.exists(4).value());
        TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
    }
}

void test_erase_leaves_a_tombstone(void)
{
    FlashHalVirtual flash(geometry(2u));
    {
        FlashLog log(flash.hal());
        TEST_ASSERT_TRUE(log.mount().isOk());
        TEST_ASSERT_EQUAL(ErrorCode::NotFound, log.erase(1).error());
        TEST_ASSERT_TRUE(log.save(1, pattern(1, 0, 8)).isOk());
        TEST_ASSERT_TRUE(log.save(2, pattern(2, 0, 8)).isOk());
        TEST_ASSERT_TRUE(log.erase(1).isOk());
        TEST_ASSERT_EQUAL(ErrorCode::NotFound, log.erase(1).error());
    }
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    TEST_ASSERT_FALSE(loadOpt(log, 1).has_value());
    std::vector<uint16_t> ids;
    TEST_ASSERT_TRUE(log.listIds(ids).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(ids.size()));
    TEST_ASSERT_EQUAL_UINT16(2u, ids[0]);
}

void test_record_larger_than_a_bank_is_refused(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, log.save(1, std::vector<uint8_t>(600, 0)).error());
    TEST_ASSERT_TRUE(log.save(1, std::vector<uint8_t>(400, 0)).isOk());  // fits one bank
}

void test_full_bank_compacts_into_the_spare_and_service_erases_the_old_one(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal(), FlashLogConfig{1u});  // no compaction ahead of need
    TEST_ASSERT_TRUE(log.mount().isOk());

    // Three ids rewritten until the bank (512 B) has overflowed once.
    for (uint8_t generation = 0; generation < 8; ++generation) {
        for (uint16_t id = 1; id <= 3; ++id) {
            TEST_ASSERT_TRUE(log.save(id, pattern(id, generation, 20)).isOk());
        }
        if (log.statistics().compactions != 0u) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1u, log.statistics().compactions);
    TEST_ASSERT_EQUAL_UINT32(2u, log.statistics().sequence);
    TEST_ASSERT_TRUE(log.servicePending());

    // One page per call.
    const uint32_t erasesBefore = log.statistics().pageErases;
    TEST_ASSERT_TRUE(log.service().isOk());
    TEST_ASSERT_EQUAL_UINT32(erasesBefore + 1u, log.statistics().pageErases);
    TEST_ASSERT_TRUE(log.servicePending());
    drainService(log);
    TEST_ASSERT_EQUAL_UINT32(erasesBefore + 2u, log.statistics().pageErases);
    TEST_ASSERT_EQUAL_UINT32(1u, flash.eraseCount(0));
    TEST_ASSERT_EQUAL_UINT32(1u, flash.eraseCount(1));

    FlashLog after(flash.hal());
    TEST_ASSERT_TRUE(after.mount().isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, after.statistics().sequence);
    for (uint16_t id = 1; id <= 3; ++id) {
        TEST_ASSERT_TRUE(loadOpt(after, id).has_value());
    }
    TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
}

void test_save_never_erases_and_waits_for_service(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal(), FlashLogConfig{1u});
    TEST_ASSERT_TRUE(log.mount().isOk());

    uint8_t generation = 0;
    while (log.statistics().compactions == 0u) {
        TEST_ASSERT_TRUE(log.save(1, pattern(1, generation++, 40)).isOk());
    }
    const uint32_t erases = log.statistics().pageErases;
    // The old bank still waits for its erase, so the next overflow cannot compact.
    util::Result<void> r = util::Result<void>::ok();
    while (r.isOk()) {
        r = log.save(1, pattern(1, generation++, 40));
    }
    TEST_ASSERT_EQUAL(ErrorCode::StorageFull, r.error());
    TEST_ASSERT_EQUAL_UINT32(erases, log.statistics().pageErases);
    TEST_ASSERT_TRUE(pattern(1, static_cast<uint8_t>(generation - 2), 40) == *loadOpt(log, 1));

    drainService(log);
    TEST_ASSERT_TRUE(log.save(1, pattern(1, generation, 40)).isOk());
    TEST_ASSERT_TRUE(pattern(1, generation, 40) == *loadOpt(log, 1));
}

void test_service_compacts_ahead_of_need(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal());  // default reserve: half a bank
    TEST_ASSERT_TRUE(log.mount().isOk());
    uint8_t generation = 0;
    while (log.statistics().freeBytes >= (512u - 12u) / 2u) {
        TEST_ASSERT_TRUE(log.save(1, pattern(1, generation++, 30)).isOk());
    }
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().compactions);  // the saves did not need one
    TEST_ASSERT_TRUE(log.service().isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, log.statistics().compactions);
    TEST_ASSERT_TRUE(pattern(1, static_cast<uint8_t>(generation - 1), 30) == *loadOpt(log, 1));
}

void test_service_does_not_compact_a_bank_full_of_live_data(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    for (uint16_t id = 1; id <= 8; ++id) {
        TEST_ASSERT_TRUE(log.save(id, pattern(id, 0, 40)).isOk());
    }
    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT_TRUE(log.service().isOk());
    }
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().compactions);
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().pageErases);
}

void test_erase_all_starts_an_empty_bank_even_before_service(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal(), FlashLogConfig{1u});
    TEST_ASSERT_TRUE(log.mount().isOk());
    uint8_t generation = 0;
    while (log.statistics().compactions == 0u) {
        TEST_ASSERT_TRUE(log.save(1, pattern(1, generation++, 40)).isOk());
        TEST_ASSERT_TRUE(log.save(2, pattern(2, generation, 10)).isOk());
    }
    TEST_ASSERT_TRUE(log.servicePending());
    TEST_ASSERT_TRUE(log.eraseAll().isOk());  // erases the spare itself
    TEST_ASSERT_FALSE(loadOpt(log, 1).has_value());
    TEST_ASSERT_FALSE(loadOpt(log, 2).has_value());

    FlashLog after(flash.hal());
    TEST_ASSERT_TRUE(after.mount().isOk());
    std::vector<uint16_t> ids;
    TEST_ASSERT_TRUE(after.listIds(ids).isOk());
    TEST_ASSERT_TRUE(ids.empty());
    TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
}

namespace {

/// Records the calls, and declines as many erases as it is told to.
struct RecordingGuard : platform::FlashEraseGuard {
    int declineNext = 0;
    int begins = 0;
    int forcedBegins = 0;
    int ends = 0;
    bool open = false;

    bool beginFlashErase(bool mustProceed) override
    {
        ++begins;
        forcedBegins += mustProceed ? 1 : 0;
        if (!mustProceed && declineNext > 0) {
            --declineNext;
            return false;
        }
        CHECK(!open, "begin twice without an end");
        open = true;
        return true;
    }

    void endFlashErase() override
    {
        CHECK(open, "end without a begin");
        open = false;
        ++ends;
    }
};

void fillUntilCompacted(FlashLog& log)
{
    uint8_t generation = 0;
    while (log.statistics().compactions == 0u) {
        TEST_ASSERT_TRUE(log.save(1, pattern(1, generation++, 40)).isOk());
        TEST_ASSERT_TRUE(log.save(2, pattern(2, generation, 10)).isOk());
    }
}

}  // namespace

void test_erase_guard_brackets_every_page_erase_and_can_postpone_it(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal(), FlashLogConfig{1u});
    RecordingGuard guard;
    log.setEraseGuard(&guard);
    TEST_ASSERT_TRUE(log.mount().isOk());
    fillUntilCompacted(log);
    TEST_ASSERT_TRUE(log.servicePending());

    // Declined: nothing erased, not an error, and the work stays pending.
    guard.declineNext = 2;
    const uint32_t erasesBefore = log.statistics().pageErases;
    TEST_ASSERT_TRUE(log.service().isOk());
    TEST_ASSERT_TRUE(log.service().isOk());
    TEST_ASSERT_EQUAL_UINT32(erasesBefore, log.statistics().pageErases);
    TEST_ASSERT_EQUAL_UINT32(2u, log.statistics().eraseDeferrals);
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().storageErrors);
    TEST_ASSERT_TRUE(log.servicePending());
    TEST_ASSERT_EQUAL_INT(0, guard.ends);

    drainService(log);
    const uint32_t erased = log.statistics().pageErases - erasesBefore;
    TEST_ASSERT_TRUE(erased > 0u);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(erased), guard.ends);
    TEST_ASSERT_FALSE(guard.open);
    TEST_ASSERT_EQUAL_INT(0, guard.forcedBegins);
}

void test_erase_all_and_formatting_go_ahead_through_the_guard(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal(), FlashLogConfig{1u});
    RecordingGuard guard;
    guard.declineNext = 100;  // would decline anything that may wait
    log.setEraseGuard(&guard);
    TEST_ASSERT_TRUE(log.mount().isOk());  // a blank region: formatting erases nothing
    fillUntilCompacted(log);
    TEST_ASSERT_TRUE(log.eraseAll().isOk());  // erases the spare itself, cannot wait
    TEST_ASSERT_TRUE(guard.forcedBegins > 0);
    TEST_ASSERT_EQUAL_INT(guard.forcedBegins, guard.ends);
    TEST_ASSERT_FALSE(guard.open);
    TEST_ASSERT_FALSE(loadOpt(log, 1).has_value());
}

void test_failed_program_closes_the_bank_and_the_next_save_compacts(void)
{
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    TEST_ASSERT_TRUE(log.save(1, pattern(1, 0, 10)).isOk());
    flash.failNextProgram();
    TEST_ASSERT_EQUAL(ErrorCode::StorageError, log.save(2, pattern(2, 0, 10)).error());
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().freeBytes);
    TEST_ASSERT_TRUE(log.save(2, pattern(2, 1, 10)).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, log.statistics().compactions);
    TEST_ASSERT_TRUE(pattern(1, 0, 10) == *loadOpt(log, 1));
    TEST_ASSERT_TRUE(pattern(2, 1, 10) == *loadOpt(log, 2));
    TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
}

void test_garbage_after_the_log_closes_the_bank_but_keeps_what_precedes_it(void)
{
    FlashHalVirtual flash(geometry(2u));
    {
        FlashLog log(flash.hal());
        TEST_ASSERT_TRUE(log.mount().isOk());
        TEST_ASSERT_TRUE(log.save(1, pattern(1, 0, 10)).isOk());
    }
    // Bank 0: header (12 B), record of 18 B at offset 12, so the log ends at 30.
    flash.memory()[30] = 0x12;
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, log.statistics().freeBytes);
    TEST_ASSERT_TRUE(pattern(1, 0, 10) == *loadOpt(log, 1));
    TEST_ASSERT_TRUE(log.save(2, pattern(2, 0, 4)).isOk());  // compacts past the garbage
    TEST_ASSERT_TRUE(pattern(1, 0, 10) == *loadOpt(log, 1));
}

// ── Power cut at every step ──────────────────────────────────────────────────

namespace {

enum class OpKind { Save, Erase, EraseAll, Service };

struct Op {
    OpKind kind;
    uint16_t id = 0;
    uint8_t generation = 0;
    uint16_t length = 0;
};

using Model = std::map<uint16_t, std::vector<uint8_t>>;

/// Saves that fill the bank more than once, an erase, service calls that
/// erase pages and compact ahead of need, and an eraseAll.
std::vector<Op> scenario()
{
    std::vector<Op> ops;
    uint8_t generation = 0;
    for (int round = 0; round < 5; ++round) {
        for (uint16_t id = 1; id <= 4; ++id) {
            ops.push_back({OpKind::Save, id, generation++, static_cast<uint16_t>(10u + 9u * id + round)});
        }
        if (round == 1) {
            ops.push_back({OpKind::Erase, 2});
        }
        if (round % 2 == 0) {
            for (int s = 0; s < 4; ++s) {
                ops.push_back({OpKind::Service});
            }
        }
    }
    ops.push_back({OpKind::Save, 9, generation++, 0});
    ops.push_back({OpKind::EraseAll});
    ops.push_back({OpKind::Save, 3, generation++, 25});
    ops.push_back({OpKind::Service});
    return ops;
}

util::Result<void> apply(FlashLog& log, const Op& op)
{
    switch (op.kind) {
    case OpKind::Save: return log.save(op.id, pattern(op.id, op.generation, op.length));
    case OpKind::Erase: return log.erase(op.id);
    case OpKind::EraseAll: return log.eraseAll();
    case OpKind::Service: return log.service();
    }
    return ErrorCode::InvalidParameter;
}

void commit(Model& model, const Op& op)
{
    switch (op.kind) {
    case OpKind::Save: model[op.id] = pattern(op.id, op.generation, op.length); break;
    case OpKind::Erase: model.erase(op.id); break;
    case OpKind::EraseAll: model.clear(); break;
    case OpKind::Service: break;
    }
}

Model readAll(FlashLog& log)
{
    Model m;
    std::vector<uint16_t> ids;
    TEST_ASSERT_TRUE(log.listIds(ids).isOk());
    for (const uint16_t id : ids) {
        m[id] = *loadOpt(log, id);
    }
    return m;
}

/// Runs the scenario with the power cut after @p cutAfter steps.
/// @return false once the scenario completed without reaching the cut.
bool runWithCut(uint32_t writeUnit, uint64_t cutAfter, uint32_t reserve)
{
    FlashHalVirtual flash(geometry(writeUnit));
    Model committed;
    std::optional<Model> alternative;  // the state if the interrupted op took effect
    bool cut = false;
    {
        FlashLog log(flash.hal(), FlashLogConfig{reserve});
        TEST_ASSERT_TRUE(log.mount().isOk());
        flash.cutPowerAfter(cutAfter);
        for (const Op& op : scenario()) {
            const auto result = apply(log, op);
            if (flash.powerCut()) {
                Model after = committed;
                commit(after, op);
                alternative = after;
                cut = true;
                break;
            }
            if (result.isOk()) {
                commit(committed, op);
            } else {
                // Without a power cut only these may fail, and they change nothing.
                CHECK(result.error() == ErrorCode::StorageFull ||
                                             (op.kind == OpKind::Erase && result.error() == ErrorCode::NotFound),
                                         "operation failed without a power cut");
            }
        }
    }

    flash.powerOn();
    FlashLog log(flash.hal(), FlashLogConfig{reserve});
    TEST_ASSERT_TRUE(log.mount().isOk());
    char message[96];
    std::snprintf(message, sizeof(message), "write unit %u, cut after %llu steps", static_cast<unsigned>(writeUnit),
                  static_cast<unsigned long long>(cutAfter));
    CHECK((0u) == (log.statistics().formats), message);
    const Model found = readAll(log);
    CHECK(found == committed || (alternative && found == *alternative), message);

    // The log stays usable after the recovery.
    drainService(log);
    CHECK(log.save(77, pattern(77, 1, 30)).isOk(), message);
    FlashLog again(flash.hal(), FlashLogConfig{reserve});
    TEST_ASSERT_TRUE(again.mount().isOk());
    Model expected = found;
    expected[77] = pattern(77, 1, 30);
    CHECK(readAll(again) == expected, message);
    CHECK((0u) == (flash.contractViolations()), message);
    return cut;
}

void powerCutEverywhere(uint32_t writeUnit, uint32_t reserve)
{
    uint64_t cutAfter = 0;
    while (runWithCut(writeUnit, cutAfter, reserve)) {
        ++cutAfter;
    }
    TEST_ASSERT_TRUE(cutAfter > 100u);  // the scenario has many steps to cut
}

}  // namespace

void test_power_cut_at_every_step_write_unit_2(void) { powerCutEverywhere(2u, 0u); }
void test_power_cut_at_every_step_write_unit_8(void) { powerCutEverywhere(8u, 0u); }
void test_power_cut_at_every_step_write_unit_1_compacting_only_when_full(void) { powerCutEverywhere(1u, 1u); }

void test_scenario_compacts_and_erases(void)
{
    // The power-cut runs are only as good as the paths the scenario reaches.
    FlashHalVirtual flash(geometry(2u));
    FlashLog log(flash.hal());
    TEST_ASSERT_TRUE(log.mount().isOk());
    for (const Op& op : scenario()) {
        (void)apply(log, op);
    }
    TEST_ASSERT_TRUE(log.statistics().compactions >= 3u);
    TEST_ASSERT_TRUE(log.statistics().pageErases >= 4u);
    TEST_ASSERT_EQUAL_UINT64(0u, flash.contractViolations());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_geometry_is_refused);
    RUN_TEST(test_blank_region_is_formatted_once);
    RUN_TEST(test_latest_record_wins_and_survives_remount);
    RUN_TEST(test_erase_leaves_a_tombstone);
    RUN_TEST(test_record_larger_than_a_bank_is_refused);
    RUN_TEST(test_full_bank_compacts_into_the_spare_and_service_erases_the_old_one);
    RUN_TEST(test_save_never_erases_and_waits_for_service);
    RUN_TEST(test_service_compacts_ahead_of_need);
    RUN_TEST(test_service_does_not_compact_a_bank_full_of_live_data);
    RUN_TEST(test_erase_all_starts_an_empty_bank_even_before_service);
    RUN_TEST(test_erase_guard_brackets_every_page_erase_and_can_postpone_it);
    RUN_TEST(test_erase_all_and_formatting_go_ahead_through_the_guard);
    RUN_TEST(test_failed_program_closes_the_bank_and_the_next_save_compacts);
    RUN_TEST(test_garbage_after_the_log_closes_the_bank_but_keeps_what_precedes_it);
    RUN_TEST(test_scenario_compacts_and_erases);
    RUN_TEST(test_power_cut_at_every_step_write_unit_2);
    RUN_TEST(test_power_cut_at_every_step_write_unit_8);
    RUN_TEST(test_power_cut_at_every_step_write_unit_1_compacting_only_when_full);
    return UNITY_END();
}
