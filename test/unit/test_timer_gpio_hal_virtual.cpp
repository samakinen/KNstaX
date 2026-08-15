// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/timer_gpio_hal.h"
#include "knx/physical/timer_gpio_hal_virtual.hpp"

using knx::physical::TimerGpioHalVirtualBus;

namespace {

struct TimerProbe {
    const knx_timer_gpio_hal_t* hal;
    int firedCount;
    uint64_t firedAtUs;
};

struct EdgeProbe {
    const knx_timer_gpio_hal_t* hal;
    int edgeCount;
    int lastLevel;
};

TimerGpioHalVirtualBus bus;
knx_timer_gpio_hal_t hal{};
TimerProbe timerProbe{};
EdgeProbe edgeProbe{};

void timerAlarmCb(void* context)
{
    auto* probe = static_cast<TimerProbe*>(context);
    ++probe->firedCount;
    probe->firedAtUs = knx_timer_gpio_hal_timer_now_us(probe->hal);
}

void rxEdgeCb(void* context)
{
    auto* probe = static_cast<EdgeProbe*>(context);
    ++probe->edgeCount;
    probe->lastLevel = knx_timer_gpio_hal_read_rx_level_fast(probe->hal);
}

} // namespace

void setUp()
{
    bus.reset();
    (void)bus.bind(hal);

    timerProbe = TimerProbe{&hal, 0, 0};
    edgeProbe = EdgeProbe{&hal, 0, -1};

    TEST_ASSERT_TRUE(knx_timer_gpio_hal_configure_pins(&hal, 4, 5, false, KNX_TIMER_GPIO_HAL_RX_EDGE_ANY));
}

void tearDown()
{
    knx_timer_gpio_hal_remove_rx_edge_isr(&hal);
    (void)knx_timer_gpio_hal_stop_timer(&hal);
}

void test_VHAL_001_timer_rearm_absolute()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_start_timer(&hal, &timerAlarmCb, &timerProbe));
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_rearm_timer_abs_us(&hal, 50));

    bus.advanceTimeUs(49);
    TEST_ASSERT_EQUAL_INT(0, timerProbe.firedCount);

    bus.advanceTimeUs(1);
    TEST_ASSERT_EQUAL_INT(1, timerProbe.firedCount);
    TEST_ASSERT_EQUAL_UINT64(50u, timerProbe.firedAtUs);
}

void test_VHAL_002_gpio_edge_irq_dispatch()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_install_rx_edge_isr(&hal, &rxEdgeCb, &edgeProbe));

    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(20, 1));
    bus.advanceTimeUs(20);

    TEST_ASSERT_EQUAL_INT(2, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_INT(1, edgeProbe.lastLevel);
}

void test_VHAL_003_irq_mask_blocks_dispatch()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_install_rx_edge_isr(&hal, &rxEdgeCb, &edgeProbe));
    bus.setRxEdgeMasked(true);

    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    bus.advanceTimeUs(10);
    TEST_ASSERT_EQUAL_INT(0, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_UINT8(0u, bus.rxLevel());

    bus.setRxEdgeMasked(false);
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(20, 1));
    bus.advanceTimeUs(10);

    TEST_ASSERT_EQUAL_INT(1, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_INT(1, edgeProbe.lastLevel);
}

void test_VHAL_004_multiple_edges_same_tick()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_install_rx_edge_isr(&hal, &rxEdgeCb, &edgeProbe));

    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 1));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    bus.advanceTimeUs(10);

    TEST_ASSERT_EQUAL_INT(3, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_INT(0, edgeProbe.lastLevel);
}

void test_VHAL_005_gpio_edge_irq_dispatch_can_be_limited_to_rising_edge()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_configure_pins(&hal, 4, 5, false, KNX_TIMER_GPIO_HAL_RX_EDGE_RISING));
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_install_rx_edge_isr(&hal, &rxEdgeCb, &edgeProbe));

    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(20, 1));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(30, 0));
    bus.advanceTimeUs(30);

    TEST_ASSERT_EQUAL_INT(1, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_INT(1, edgeProbe.lastLevel);
}

void test_VHAL_006_gpio_edge_irq_dispatch_can_be_limited_to_falling_edge()
{
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_configure_pins(&hal, 4, 5, false, KNX_TIMER_GPIO_HAL_RX_EDGE_FALLING));
    TEST_ASSERT_TRUE(knx_timer_gpio_hal_install_rx_edge_isr(&hal, &rxEdgeCb, &edgeProbe));

    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(10, 0));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(20, 1));
    TEST_ASSERT_TRUE(bus.scheduleRxLevelAtUs(30, 0));
    bus.advanceTimeUs(30);

    TEST_ASSERT_EQUAL_INT(2, edgeProbe.edgeCount);
    TEST_ASSERT_EQUAL_INT(0, edgeProbe.lastLevel);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VHAL_001_timer_rearm_absolute);
    RUN_TEST(test_VHAL_002_gpio_edge_irq_dispatch);
    RUN_TEST(test_VHAL_003_irq_mask_blocks_dispatch);
    RUN_TEST(test_VHAL_004_multiple_edges_same_tick);
    RUN_TEST(test_VHAL_005_gpio_edge_irq_dispatch_can_be_limited_to_rising_edge);
    RUN_TEST(test_VHAL_006_gpio_edge_irq_dispatch_can_be_limited_to_falling_edge);
    return UNITY_END();
}
