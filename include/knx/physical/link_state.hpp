// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file link_state.hpp
 * @brief Medium-neutral link-health contract for KNX physical layers
 *
 * A KNX device usually has a way to know whether the medium underneath it is
 * actually usable, independently of whether anything is being received:
 *
 *   - TP1 bitbang (STKNX):  KNX_OK / bus-voltage-good output of the transceiver
 *   - TP1 TPUART2:          SAVE pin, plus the U_State.indication warning bits
 *   - KNXnet/IP tunneling:  interface up + tunnel connection alive
 *   - KNXnet/IP routing:    interface up + multicast group joined
 *
 * Without such a signal a dead bus and a quiet bus are indistinguishable: the
 * medium simply reads recessive and every transmission burns its full CSMA +
 * retransmission budget before failing. This header defines the vocabulary all
 * media report that condition in, so the layers above never have to know which
 * of the above produced it.
 *
 * The contract is deliberately default-inert: a backend that cannot tell
 * reports LinkState::Unknown, and Unknown never changes stack behaviour.
 */

#pragma once

#include "knx/util/inplace_function.hpp"

#include <cstdint>

namespace knx {
namespace physical {

/**
 * @brief Health of the medium underneath a physical layer
 *
 * Only an explicit Down is ever acted upon by the stack. Unknown means "this
 * medium has no way to tell", which is the default for every backend that does
 * not implement the indication.
 */
enum class LinkState : uint8_t {
    Unknown = 0,  ///< No indication available — treated as "assume usable"
    Down,         ///< Medium unusable (bus power lost, interface/tunnel down)
    Degraded,     ///< Usable but impaired (transceiver warning, marginal supply)
    Up,           ///< Medium healthy
};

[[nodiscard]] inline constexpr const char* linkStateToString(LinkState state) noexcept {
    switch (state) {
        case LinkState::Unknown:  return "Unknown";
        case LinkState::Down:     return "Down";
        case LinkState::Degraded: return "Degraded";
        case LinkState::Up:       return "Up";
        default:                  return "?";
    }
}

/**
 * @brief Why a link notification was raised
 */
enum class LinkEventKind : uint8_t {
    /**
     * Debounced steady-state transition, delivered from task context. This is
     * the only kind stack or application policy should be driven from.
     */
    StateChanged = 0,

    /**
     * Raw, un-debounced loss edge, delivered as early as the hardware can
     * report it — the "power is about to go" trigger.
     *
     * Whether this is useful is purely a property of the board: it matters only
     * if the 3V3 rail outlives the bus by less than the debounce window. Boards
     * with seconds of bulk-capacitor hold-up can ignore it entirely and act on
     * the debounced StateChanged instead; boards with a hold-up time measured
     * in milliseconds need it to have any save window at all.
     *
     * @see LinkSignalConfig::notifyPowerFailFromIsr
     */
    PowerFailImminent,
};

/**
 * @brief A link-health notification
 */
struct LinkStatus {
    LinkState state{LinkState::Unknown};
    LinkEventKind kind{LinkEventKind::StateChanged};

    /// Debounced transitions observed since the monitor was configured. Lets a
    /// consumer distinguish "was always up" from "flapped and recovered".
    uint32_t transitionCount{0};

    /// Timestamp of the sample that produced this status, in the medium's own
    /// microsecond time domain (for the bitbang driver: the same free-running
    /// timer the ISR uses, so it is directly comparable to bus timestamps).
    uint64_t timestampUs{0};
};

/**
 * @brief Task-context link notification callback
 */
using LinkStateCallback = util::InplaceFunction<void(const LinkStatus&, void*), 32>;

/**
 * @brief Interrupt-context notification of an imminent supply loss
 *
 * Called directly from the edge interrupt of the hardware signal, before any
 * debouncing, so that boards with a short hold-up time get the maximum possible
 * save window. It runs under all the usual ISR restrictions — on ESP-IDF it
 * must be IRAM-resident and must not log, block, or touch flash. The intended
 * body is a few instructions that hand off to a high-priority task (give a
 * semaphore, set a flag, notify a task).
 *
 * The signal is advisory: it says the transceiver's supply comparator tripped,
 * not that the rail has collapsed. It can also fire spuriously on a marginal
 * bus, so a handler must be safe to run when power then does not go away.
 */
using LinkPowerFailIsrHandler = void (*)(void* context, uint64_t timestampUs);

/**
 * @brief Configuration of a hardware link-health input
 *
 * Polarity, thresholds and hold-up time are board properties, not stack
 * properties, so all of them are configuration rather than constants.
 */
struct LinkSignalConfig {
    /// GPIO carrying the signal; 0xFF means no such signal is wired.
    uint8_t pin{0xFF};

    /// true when the asserted (healthy) state drives the pin high.
    bool activeHigh{true};

    /// Enable an internal pull-up (needed for open-drain transceiver outputs).
    bool enablePullup{false};

    /**
     * Time the signal must stay deasserted before Down is reported.
     *
     * Bus voltage sags under heavy traffic and during the device's own dominant
     * pulses, so a comparator output can chatter; this window filters that out.
     * It also bounds how late the stack learns about a real outage, which is
     * why it must stay well inside the board's hold-up time.
     */
    uint32_t downDebounceUs{20000};

    /// Time the signal must stay asserted before Up is reported. Longer than
    /// the down window on purpose: bus power return is bouncy, and declaring Up
    /// too early re-enables transmission into a supply that is still settling.
    uint32_t upDebounceUs{200000};

    /// Emit LinkEventKind::PowerFailImminent from the edge ISR, ahead of the
    /// debounce. Only useful when a handler is registered.
    bool notifyPowerFailFromIsr{false};

    [[nodiscard]] constexpr bool isConfigured() const noexcept { return pin != 0xFF; }
};

} // namespace physical
} // namespace knx
