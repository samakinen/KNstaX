// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/virtual_time_engine.hpp"

#include <vector>

using knx::physical::VirtualTimeEngine;

namespace {

struct CallbackState {
    VirtualTimeEngine* engine;
    std::vector<int>* order;
    int value;
    uint64_t when;
};

void pushValueCallback(void* context)
{
    auto* state = static_cast<CallbackState*>(context);
    state->order->push_back(state->value);
}

void scheduleAnotherCallback(void* context)
{
    auto* state = static_cast<CallbackState*>(context);
    state->order->push_back(state->value);
    static CallbackState followUp{};
    followUp.engine = state->engine;
    followUp.order = state->order;
    followUp.value = 99;
    followUp.when = state->when + 10;
    (void)state->engine->scheduleAt(followUp.when,
                                    VirtualTimeEngine::Priority::DeferredTask,
                                    &pushValueCallback,
                                    &followUp);
}

} // namespace

void setUp() {}
void tearDown() {}

void test_VTIME_001_monotonic_now()
{
    VirtualTimeEngine engine;
    TEST_ASSERT_EQUAL_UINT64(0u, engine.nowUs());

    engine.runUntil(42);
    TEST_ASSERT_EQUAL_UINT64(42u, engine.nowUs());

    engine.runUntil(7);
    TEST_ASSERT_EQUAL_UINT64(42u, engine.nowUs());
}

void test_VTIME_002_same_tick_stable_order()
{
    VirtualTimeEngine engine;
    std::vector<int> order;

    CallbackState a{&engine, &order, 1, 10};
    CallbackState b{&engine, &order, 2, 10};
    CallbackState c{&engine, &order, 3, 10};

    (void)engine.scheduleAt(a.when, VirtualTimeEngine::Priority::GpioEdge, &pushValueCallback, &a);
    (void)engine.scheduleAt(b.when, VirtualTimeEngine::Priority::GpioEdge, &pushValueCallback, &b);
    (void)engine.scheduleAt(c.when, VirtualTimeEngine::Priority::GpioEdge, &pushValueCallback, &c);

    engine.runIdle();

    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(order.size()));
    TEST_ASSERT_EQUAL_INT(1, order[0]);
    TEST_ASSERT_EQUAL_INT(2, order[1]);
    TEST_ASSERT_EQUAL_INT(3, order[2]);
}

void test_VTIME_003_cancel_before_dispatch()
{
    VirtualTimeEngine engine;
    std::vector<int> order;

    CallbackState keep{&engine, &order, 1, 10};
    CallbackState drop{&engine, &order, 2, 10};

    (void)engine.scheduleAt(keep.when,
                            VirtualTimeEngine::Priority::DeferredTask,
                            &pushValueCallback,
                            &keep);
    const auto dropId = engine.scheduleAt(drop.when,
                                          VirtualTimeEngine::Priority::DeferredTask,
                                          &pushValueCallback,
                                          &drop);
    TEST_ASSERT_TRUE(engine.cancel(dropId));
    TEST_ASSERT_FALSE(engine.cancel(dropId));

    engine.runIdle();

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(order.size()));
    TEST_ASSERT_EQUAL_INT(1, order[0]);
}

void test_VTIME_004_callback_schedules_future()
{
    VirtualTimeEngine engine;
    std::vector<int> order;

    CallbackState first{&engine, &order, 7, 15};
    (void)engine.scheduleAt(first.when,
                            VirtualTimeEngine::Priority::DeferredTask,
                            &scheduleAnotherCallback,
                            &first);

    engine.runIdle();

    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(order.size()));
    TEST_ASSERT_EQUAL_INT(7, order[0]);
    TEST_ASSERT_EQUAL_INT(99, order[1]);
    TEST_ASSERT_EQUAL_UINT64(25u, engine.nowUs());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTIME_001_monotonic_now);
    RUN_TEST(test_VTIME_002_same_tick_stable_order);
    RUN_TEST(test_VTIME_003_cancel_before_dispatch);
    RUN_TEST(test_VTIME_004_callback_schedules_future);
    return UNITY_END();
}
