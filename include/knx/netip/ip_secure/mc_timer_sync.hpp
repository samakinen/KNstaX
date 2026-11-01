// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file mc_timer_sync.hpp
 * @brief The Secure Routing multicast timer and its synchronisation state machine.
 *
 * KNX IP Secure Routing has no per-sender sequence numbers. Its replay
 * protection is a 48-bit millisecond timer, mc_timer, that every member of the
 * multicast group keeps and that only ever moves forward (03_08_09 §2.2.2.2).
 * A sender stamps each SECURE_WRAPPER with its mc_timer; a receiver adopts any
 * later value it sees and discards frames older than the latency tolerance.
 * TIMER_NOTIFY frames keep the timers aligned while nothing else is sent.
 *
 * This class is that state machine and nothing else — §2.2.2.3.2, events E01
 * to E11 and actions A0 to A9 — with no crypto, no socket and no clock of its
 * own. Every call takes the current time, which is what lets the host tests
 * walk it through every transition at simulated time.
 *
 * The timer is kept as a value taken at a local instant plus the local time
 * elapsed since, so it advances with the clock between calls and can only be
 * moved forward (A1) or reset with the key (A7).
 *
 * @thread_safety Not thread-safe. The owner (SecureRoutingSecurity) serialises
 *                the send and receive paths around it.
 */

#include "knx/util/inplace_function.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace knx {
namespace netip {
namespace ip_secure {

class McTimerSync {
public:
    using Serial = std::array<uint8_t, 6>;
    using Tag = std::array<uint8_t, 2>;
    /// Uniform 32-bit draws for random(start, end) (§2.2.2.3.2.6, NOTE 1) and
    /// for the random tag of an own TIMER_NOTIFY (A5).
    using RandomSource = util::InplaceFunction<uint32_t(), 32>;

    static constexpr uint64_t kTimerMask = 0xFFFF'FFFF'FFFFULL;

    /// PID_MULTICAST_LATENCY_TOLERANCE ex-factory value (03_08_09 §2.3.1.6.3).
    static constexpr uint16_t kDefaultLatencyToleranceMs = 2000;
    /// PID_SYNC_LATENCY_FRACTION ex-factory value: PDT_SCALING 1Ah, 10.2 %
    /// (§2.3.1.7.3). The parameter table in §2.2.2.3.2.2 rounds this to 10 %.
    static constexpr uint8_t kDefaultSyncLatencyFraction = 0x1A;

    // Fixed parameters of §2.2.2.3.2.2.
    static constexpr uint32_t kMaxDelayInitialNotifyMs = 10'000;
    static constexpr uint32_t kMinDelayTimeKeeperPeriodicNotifyMs = 10'000;
    static constexpr uint32_t kMinDelayTimeKeeperUpdateNotifyMs = 100;

    /// State labels of §2.2.2.3.2.4.
    enum class State : uint8_t {
        SchedPeriodic,
        SchedUpdate,
    };

    /// Where a received timer value falls relative to the local mc_timer
    /// (Figure 10). The same four bands give E01–E04 for TIMER_NOTIFY and
    /// E05–E08 for SECURE_WRAPPER.
    enum class Band : uint8_t {
        Newer,     ///< received > mc_timer                              (E01/E05)
        InSync,    ///< within syncLatencyTolerance behind               (E02/E06)
        Late,      ///< within latencyTolerance behind, but beyond sync  (E03/E07)
        Outdated,  ///< latencyTolerance or more behind                  (E04/E08)
    };

    /// Whether the start is a power-up (initial notify after a random delay of
    /// up to 10 s) or a reconfiguration (notify at once), §2.2.2.3.2.8.
    enum class StartReason : uint8_t {
        PowerUp,
        Reconfigured,
    };

    /// A TIMER_NOTIFY the owner must send now (E10 → A5 or A6).
    struct Notify {
        uint64_t timer{0};
        Serial serial{};
        Tag tag{};
        /// True for A6, the answer to an outdated frame, which repeats that
        /// frame's serial number and tag.
        bool update{false};
    };

    McTimerSync(const Serial& ownSerial, RandomSource random);

    /**
     * Start (or restart, A7) the synchronisation (§2.2.2.3.2.8).
     *
     * `initialTimer` is the persisted value plus the worst-case offset, or 0
     * the first time this key is ever used. State becomes SCHED_PERIODIC and
     * the device starts as a time follower.
     */
    void start(uint32_t nowMs, uint64_t initialTimer, StartReason reason);

    /// Apply PID_MULTICAST_LATENCY_TOLERANCE and PID_SYNC_LATENCY_FRACTION.
    /// Takes effect on the next event; a scheduled notify keeps its deadline.
    void setLatencyTolerance(uint16_t latencyToleranceMs, uint8_t syncLatencyFraction);

    /// The current mc_timer.
    [[nodiscard]] uint64_t timer(uint32_t nowMs);

    /// Which band a received timer value is in, without acting on it.
    [[nodiscard]] Band classify(uint64_t receivedTimer, uint32_t nowMs);

    /// E01–E04: an authentic TIMER_NOTIFY arrived.
    void onTimerNotify(uint64_t receivedTimer, const Serial& serial, const Tag& tag, uint32_t nowMs);

    /// E05–E08: an authentic SECURE_WRAPPER arrived. Returns whether it is to
    /// be passed up (A2); an outdated frame (E08) is not.
    [[nodiscard]] bool onSecureWrapper(uint64_t receivedTimer, const Serial& serial, const Tag& tag,
                                       uint32_t nowMs);

    /// E09: a SECURE_WRAPPER is about to be sent. Returns the timer to stamp it with.
    [[nodiscard]] uint64_t onTransmit(uint32_t nowMs);

    /// E10: when notify_timer has expired, the TIMER_NOTIFY to send now.
    [[nodiscard]] std::optional<Notify> poll(uint32_t nowMs);

    /// Milliseconds until poll() will next return a notify; 0 when it is due.
    [[nodiscard]] uint32_t millisecondsUntilNotify(uint32_t nowMs);

    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool isTimeKeeper() const noexcept { return timeKeeper_; }
    [[nodiscard]] uint32_t latencyToleranceMs() const noexcept { return latencyToleranceMs_; }
    [[nodiscard]] uint32_t syncLatencyToleranceMs() const noexcept { return syncLatencyToleranceMs_; }

    /// The [min, max] window A3 and A4 currently draw from, for the tests.
    struct Window {
        uint32_t minMs{0};
        uint32_t maxMs{0};
    };
    [[nodiscard]] Window periodicWindow() const noexcept;
    [[nodiscard]] Window updateWindow() const noexcept;

private:
    uint64_t advance(uint32_t nowMs);
    void setTimer(uint64_t value);
    uint32_t randomBetween(uint32_t minMs, uint32_t maxMs);
    void reschedulePeriodic();  // A3
    void scheduleUpdate(const Serial& serial, const Tag& tag);  // A4

    Serial ownSerial_{};
    RandomSource random_;

    uint32_t latencyToleranceMs_{kDefaultLatencyToleranceMs};
    uint32_t syncLatencyToleranceMs_{0};

    // Local monotonic milliseconds, widened from the 32-bit clock the caller
    // passes in. Wraps of that clock are absorbed as long as calls come more
    // often than every 49 days, which the periodic notify guarantees.
    bool clockStarted_{false};
    uint32_t lastNowMs_{0};
    uint64_t localMs_{0};

    // mc_timer = timerBase_ + (localMs_ - localBase_)
    uint64_t timerBase_{0};
    uint64_t localBase_{0};

    State state_{State::SchedPeriodic};
    bool timeKeeper_{false};
    uint64_t notifyAtLocalMs_{0};

    Serial rememberedSerial_{};
    Tag rememberedTag_{};
};

} // namespace ip_secure
} // namespace netip
} // namespace knx
