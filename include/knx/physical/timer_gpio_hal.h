// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define KNX_TIMER_GPIO_HAL_ISR_ATTR IRAM_ATTR
#else
#define KNX_TIMER_GPIO_HAL_ISR_ATTR
#endif

// Pairs with KNX_TIMER_GPIO_HAL_ISR_ATTR on out-of-line ISR helpers that are
// called with compile-time constant arguments.
//
// IRAM_ATTR expands to an explicit section("...") attribute. At -Os/-O2, IPA-CP
// clones such a function per constant argument set, and each clone inherits the
// section name while landing in a different section group than the original —
// which GCC rejects with "causes a section type conflict with itself". Blocking
// the clone is the cheap side of that trade: the callers are ISR paths where
// IRAM footprint matters more than a specialised copy per call site.
#if defined(__GNUC__) && !defined(__clang__)
#define KNX_TIMER_GPIO_HAL_ISR_NOCLONE_ATTR __attribute__((noclone))
#else
#define KNX_TIMER_GPIO_HAL_ISR_NOCLONE_ATTR
#endif

typedef void (*knx_timer_gpio_hal_timer_alarm_cb_t)(void* context);
typedef void (*knx_timer_gpio_hal_gpio_edge_isr_t)(void* context);

typedef enum {
    KNX_TIMER_GPIO_HAL_RX_EDGE_ANY = 0,
    KNX_TIMER_GPIO_HAL_RX_EDGE_RISING = 1,
    KNX_TIMER_GPIO_HAL_RX_EDGE_FALLING = 2,
} knx_timer_gpio_hal_rx_edge_t;

typedef struct {
    bool (*configure_pins)(void* context,
                           int tx_pin,
                           int rx_pin,
                           bool enable_pullup,
                           knx_timer_gpio_hal_rx_edge_t rx_edge);
    bool (*install_rx_edge_isr)(void* context,
                                knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                void* isr_context);
    void (*remove_rx_edge_isr)(void* context);

    bool (*start_timer)(void* context,
                        knx_timer_gpio_hal_timer_alarm_cb_t alarm_cb,
                        void* alarm_context);
    bool (*stop_timer)(void* context);
    bool (*rearm_timer_abs_us)(void* context, uint64_t alarm_time_us);
    uint64_t (*timer_now_us)(void* context);

    void (*set_tx_high_fast)(void* context);
    void (*set_tx_low_fast)(void* context);
    int (*read_rx_level_fast)(void* context);

    /* Optional link-health status input (STKNX KNX_OK, TPUART SAVE, ...).
     * These four may be NULL: a HAL without them simply has no such signal,
     * and knx_timer_gpio_hal_has_status_pin() reports false. They are excluded
     * from knx_timer_gpio_hal_is_valid() on purpose so that existing HAL
     * implementations stay valid without providing them.
     *
     * read_status_level_fast returns the raw pin level; polarity is applied by
     * the caller, which is the layer that knows the board's wiring. */
    bool (*configure_status_pin)(void* context, int status_pin, bool enable_pullup);
    bool (*install_status_edge_isr)(void* context,
                                    knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                    void* isr_context);
    void (*remove_status_edge_isr)(void* context);
    int (*read_status_level_fast)(void* context);
} knx_timer_gpio_hal_ops_t;

typedef struct {
    void* context;
    knx_timer_gpio_hal_ops_t ops;
} knx_timer_gpio_hal_t;

bool knx_timer_gpio_hal_make_espidf(knx_timer_gpio_hal_t* out_hal);
bool knx_timer_gpio_hal_destroy_espidf(knx_timer_gpio_hal_t* hal);

// Guard used by every *_fast/ISR helper below, so it inherits their IRAM
// requirement whenever the compiler emits it out of line.
static inline bool KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_is_valid(const knx_timer_gpio_hal_t* hal)
{
    return hal && hal->context && hal->ops.configure_pins && hal->ops.install_rx_edge_isr &&
           hal->ops.remove_rx_edge_isr && hal->ops.start_timer && hal->ops.stop_timer &&
           hal->ops.rearm_timer_abs_us && hal->ops.timer_now_us && hal->ops.set_tx_high_fast &&
           hal->ops.set_tx_low_fast && hal->ops.read_rx_level_fast;
}

static inline bool knx_timer_gpio_hal_configure_pins(const knx_timer_gpio_hal_t* hal,
                                                     int tx_pin,
                                                     int rx_pin,
                                                     bool enable_pullup,
                                                     knx_timer_gpio_hal_rx_edge_t rx_edge)
{
    return knx_timer_gpio_hal_is_valid(hal) &&
           hal->ops.configure_pins(hal->context, tx_pin, rx_pin, enable_pullup, rx_edge);
}

static inline bool knx_timer_gpio_hal_install_rx_edge_isr(const knx_timer_gpio_hal_t* hal,
                                                          knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                                          void* isr_context)
{
    return knx_timer_gpio_hal_is_valid(hal) &&
           hal->ops.install_rx_edge_isr(hal->context, isr, isr_context);
}

static inline void knx_timer_gpio_hal_remove_rx_edge_isr(const knx_timer_gpio_hal_t* hal)
{
    if (knx_timer_gpio_hal_is_valid(hal)) {
        hal->ops.remove_rx_edge_isr(hal->context);
    }
}

static inline bool knx_timer_gpio_hal_start_timer(const knx_timer_gpio_hal_t* hal,
                                                  knx_timer_gpio_hal_timer_alarm_cb_t alarm_cb,
                                                  void* alarm_context)
{
    return knx_timer_gpio_hal_is_valid(hal) &&
           hal->ops.start_timer(hal->context, alarm_cb, alarm_context);
}

static inline bool knx_timer_gpio_hal_stop_timer(const knx_timer_gpio_hal_t* hal)
{
    return knx_timer_gpio_hal_is_valid(hal) && hal->ops.stop_timer(hal->context);
}

// Reached from interrupt context (bit-timing alarm rearm, link-signal edge ISR),
// so any copy the compiler emits instead of inlining must live in IRAM — the
// driver's interrupts stay enabled while the flash cache is off.
static inline bool KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_rearm_timer_abs_us(
                                                         const knx_timer_gpio_hal_t* hal,
                                                         uint64_t alarm_time_us)
{
    return knx_timer_gpio_hal_is_valid(hal) &&
           hal->ops.rearm_timer_abs_us(hal->context, alarm_time_us);
}

static inline uint64_t KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_timer_now_us(const knx_timer_gpio_hal_t* hal)
{
    if (!knx_timer_gpio_hal_is_valid(hal)) {
        return 0;
    }
    return hal->ops.timer_now_us(hal->context);
}

static inline void KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_set_tx_high_fast(const knx_timer_gpio_hal_t* hal)
{
    if (knx_timer_gpio_hal_is_valid(hal)) {
        hal->ops.set_tx_high_fast(hal->context);
    }
}

static inline void KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_set_tx_low_fast(const knx_timer_gpio_hal_t* hal)
{
    if (knx_timer_gpio_hal_is_valid(hal)) {
        hal->ops.set_tx_low_fast(hal->context);
    }
}

static inline int KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_read_rx_level_fast(const knx_timer_gpio_hal_t* hal)
{
    if (!knx_timer_gpio_hal_is_valid(hal)) {
        return 0;
    }
    return hal->ops.read_rx_level_fast(hal->context);
}

static inline bool knx_timer_gpio_hal_has_status_pin(const knx_timer_gpio_hal_t* hal)
{
    return knx_timer_gpio_hal_is_valid(hal) && hal->ops.configure_status_pin &&
           hal->ops.install_status_edge_isr && hal->ops.remove_status_edge_isr &&
           hal->ops.read_status_level_fast;
}

static inline bool knx_timer_gpio_hal_configure_status_pin(const knx_timer_gpio_hal_t* hal,
                                                           int status_pin,
                                                           bool enable_pullup)
{
    return knx_timer_gpio_hal_has_status_pin(hal) &&
           hal->ops.configure_status_pin(hal->context, status_pin, enable_pullup);
}

static inline bool knx_timer_gpio_hal_install_status_edge_isr(const knx_timer_gpio_hal_t* hal,
                                                              knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                                              void* isr_context)
{
    return knx_timer_gpio_hal_has_status_pin(hal) &&
           hal->ops.install_status_edge_isr(hal->context, isr, isr_context);
}

static inline void knx_timer_gpio_hal_remove_status_edge_isr(const knx_timer_gpio_hal_t* hal)
{
    if (knx_timer_gpio_hal_has_status_pin(hal)) {
        hal->ops.remove_status_edge_isr(hal->context);
    }
}

static inline int KNX_TIMER_GPIO_HAL_ISR_ATTR knx_timer_gpio_hal_read_status_level_fast(const knx_timer_gpio_hal_t* hal)
{
    if (!knx_timer_gpio_hal_has_status_pin(hal)) {
        return 0;
    }
    return hal->ops.read_status_level_fast(hal->context);
}

#ifdef __cplusplus
}
#endif
